#include "DynamicEqGraph.h"
#include "DotMatrix.h"
#include "InstrumentProcessor.h"
#include "ProgrammerLook.h"
#include "UserReach.h"
#include "SpectrumPeaks.h"
#include "DynamicEqEdits.h"
#include "EqComparison.h"
#include "WheelLatch.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace airack::studio {
namespace {
constexpr double logSpan=6.907755278982137;
const juce::Colour paper(0xffcad1d6),grid(0xff30383b),curve(0xff87d8bd),inputInk(0xff7193b7),rangeInk(0xffe7b474);
float xFor(juce::Rectangle<float> r,double hz){return r.getX()+r.getWidth()*float(std::log(std::clamp(hz,20.,20000.)/20.)/logSpan);}
float yFor(juce::Rectangle<float> r,double db){return r.getCentreY()-r.getHeight()*float(std::clamp(db,-30.,30.)/60.);}
double frequencyAt(juce::Rectangle<float> r,float x){return 20*std::exp(logSpan*std::clamp(double((x-r.getX())/std::max(1.f,r.getWidth())),0.,1.));}
float gainAt(juce::Rectangle<float> r,float y){return std::clamp((r.getCentreY()-y)*60/std::max(1.f,r.getHeight()),-24.f,24.f);}
double decibels(std::complex<double> h){return 20*std::log10(std::max(std::abs(h),1e-12));}
bool hasDynamics(DynamicEqShape shape){return shape<=DynamicEqShape::highShelf;}
bool hasGain(DynamicEqShape shape){return hasDynamics(shape)||shape==DynamicEqShape::tiltShelf||shape==DynamicEqShape::flatTilt;}
bool hasQ(DynamicEqShape shape){return hasDynamics(shape)||shape==DynamicEqShape::notch||shape==DynamicEqShape::bandPass||shape==DynamicEqShape::tiltShelf;}
bool hasComponentBands(const DynamicEqSettings& settings){return std::any_of(settings.bands.begin(),settings.bands.end(),[](const auto& b){return b.active&&!b.bypassed&&b.placement!=DynamicEqPlacement::stereo;});}
const char* placementName(DynamicEqPlacement placement){const char* names[]{"Stereo","Left","Right","Mid","Side"};return placement<=DynamicEqPlacement::side?names[unsigned(placement)]:"Unknown";}
std::optional<std::complex<double>> componentTransfer(const DynamicEqSettings& settings,double rate,double hz,
    DynamicEqPlacement component,const std::array<float,8>& movement={}){
    if(component>DynamicEqPlacement::side)return {};
    if(component==DynamicEqPlacement::stereo){
        if(!hasComponentBands(settings))return dynamicEqResponse(settings,rate,hz,movement);
        const auto energy=dynamicEqEnergyResponse(settings,rate,hz,movement);
        if(!energy)return {};return std::complex<double>(*energy,0);
    }
    const auto matrix=dynamicEqMatrixResponse(settings,rate,hz,movement);if(!matrix)return {};
    if(component==DynamicEqPlacement::left)return (*matrix)[0];
    if(component==DynamicEqPlacement::right)return (*matrix)[3];
    if(component==DynamicEqPlacement::mid)return ((*matrix)[0]+(*matrix)[1]+(*matrix)[2]+(*matrix)[3])*.5;
    return ((*matrix)[0]-(*matrix)[1]-(*matrix)[2]+(*matrix)[3])*.5;
}
juce::Uuid rackOwner(const SoundDocument& sound,std::uint8_t slot){
 if(sound.personalities)for(const auto& instance:sound.personalities->instances)for(const auto& unit:instance.design.units)
  if(unit.module==NativeModule::dynamicEq)if(const auto* binding=rackUnitBinding(instance,unit.id);binding&&binding->slot==slot)return instance.id;
 return juce::Uuid::null();
}
struct ListenKey final:KeyFocus<juce::TextButton> {
 using KeyFocus<juce::TextButton>::KeyFocus;
 bool keyboardHeld=false;
 bool keyPressed(const juce::KeyPress& key) override {
  if(isEnabled()&&(key.getKeyCode()==juce::KeyPress::returnKey||key.getKeyCode()==juce::KeyPress::spaceKey)){keyboardHeld=true;setState(buttonDown);return true;}
  return KeyFocus<juce::TextButton>::keyPressed(key);
 }
 bool keyStateChanged(bool) override {
  if(!keyboardHeld)return false;
  if(!juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::returnKey)&&!juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey)){keyboardHeld=false;setState(buttonNormal);}
  return true;
 }
 void focusLost(FocusChangeType cause) override {keyboardHeld=false;setState(buttonNormal);KeyFocus<juce::TextButton>::focusLost(cause);}
};
// One flat display surface: the surrounding chassis keeps its hardware style.
struct GraphKeyLook final:juce::LookAndFeel_V4 {
 void drawButtonBackground(juce::Graphics& g,juce::Button& b,const juce::Colour&,bool over,bool down) override {
  const auto r=b.getLocalBounds().toFloat().reduced(1);
  if(b.getToggleState()||over||down){g.setColour(curve.withAlpha(down?.24f:b.getToggleState()?.14f:.06f));g.fillRoundedRectangle(r,3);}
  if(ProgrammerLook::ringed(b)){g.setColour(juce::Colour(0xffb3d4ff));g.drawRoundedRectangle(r.reduced(2),2,1.5f);}
 }
 void drawButtonText(juce::Graphics& g,juce::TextButton& b,bool,bool) override {
  g.setFont(juce::FontOptions(14));g.setColour((b.getToggleState()?curve:paper).withAlpha(b.isEnabled()?1.f:.35f));g.drawFittedText(b.getButtonText(),b.getLocalBounds().reduced(3,1),juce::Justification::centred,1);
 }
 void drawLinearSlider(juce::Graphics& g,int x,int y,int width,int height,float position,float,float,juce::Slider::SliderStyle,juce::Slider& slider) override {
  const float cy=y+height*.5f;g.setColour(grid);g.drawLine(float(x),cy,float(x+width),cy,2);
  g.setColour(curve.withAlpha(slider.isEnabled()?1.f:.35f));g.drawLine(float(x),cy,position,cy,2);g.fillEllipse(position-4,cy-4,8,8);
 }
};
}
struct DynamicEqGraph::State : std::enable_shared_from_this<State> {
    DynamicEqGraph& view;InstrumentProcessor& processor;house::Host host;Clipboard clipboard;
    std::shared_ptr<FxMonitor> monitor;std::shared_ptr<FxAnalysisHub> hub;bool privateHub=false;std::optional<FxAnalysisHub::Lease> token;
    const FxSpectrum::Result& analysisResult() const {static const FxSpectrum::Result empty;const auto* result=token?hub->result(*token):nullptr;return result?*result:empty;}
    std::uint64_t shownGeneration=0,shownWindows=0;
    std::array<float,8> shownMovement{};
    SoundDocument sound;DynamicEqSettings settings,base;std::optional<DynamicEqSettings> proposal;
    std::shared_ptr<const SoundDocument> candidate;
    std::optional<std::uint8_t> slot;
    EqComparison comparison;
    bool comparisonBankB=false;
    std::uint64_t comparisonGeneration=0;
    std::uint64_t menuGeneration=0;
    bool menuOpen=false;
    GraphKeyLook keyLook;
    juce::Image displayRim;
    KeyFocus<juce::TextButton> grab{"Grab"},more{"More"};
    KeyFocus<juce::TextButton> compareA{"A"},compareB{"B"},compareCopy{"Copy A > B"},autoGain{"Auto Gain"};
    KeyFocus<juce::TextButton> sketchApply{"Apply"},sketchCancel{"Cancel"};
    KeyFocus<juce::Slider> gainScale;
    juce::Label scaleLabel,compensationLabel;
    ListenKey listen{"Listen"};
    std::unique_ptr<FxSpectrum::Result> frozen;
    std::shared_ptr<const SoundDocument> grabDocument;
    SpectrumPeaks grabPeaks;
    std::uint64_t grabRevision=0,grabReset=0,grabDropped=0;
    std::size_t grabSelected=0;
    float grabGain=0,grabStartY=0;
    bool grabbing=false,grabDragging=false,grabPreview=false;
    juce::String grabStatus,editStatus;
    std::size_t selected=0,leader=parameterCount,dragBand=0;
    std::bitset<8> selection,gestureBands,marqueeBefore;
    struct EditContext {SoundDocument document;std::uint64_t revision=0,reset=0,binding=0;std::uint8_t slot=0;};
    struct SketchJob {
        std::shared_ptr<const EditContext> context;
        EqCurve target;
        DynamicEqSettings source;
        std::bitset<8> available;
        double rate=48000;
        EqFitResult result;
        std::atomic_bool cancel{false},done{false};
        std::thread worker;
        void start(){worker=std::thread([this]{
            result=fitEqCurve(target,source,available,rate,&cancel);
            done.store(true,std::memory_order_release);
        });}
        ~SketchJob(){cancel.store(true,std::memory_order_relaxed);if(worker.joinable())worker.join();}
    };
    std::unique_ptr<SketchJob> sketchJob;
    std::shared_ptr<const EditContext> sketchContext,sketchStrokeContext;
    std::optional<EqCurve> sketchTarget;
    std::optional<EqCurve> matchBase,matchFit;
    std::optional<DynamicEqSettings> sketchFit;
    std::optional<float> sketchError;
    std::bitset<8> sketchAdded;
    std::array<juce::Point<float>,256> sketchPoints{};
    std::size_t sketchPointsSize=0;
    bool sketching=false,sketchDrawing=false,sketchApplying=false,matching=false;
    juce::String sketchStatus;
    std::shared_ptr<const EditContext> groupExpected;
    DynamicEqSettings groupBase;
    juce::Point<float> groupPointer,marqueeStart,marqueeEnd;
    std::uint64_t bindingGeneration=0,observedReset=0;
    bool groupGesture=false,marquee=false,resetKnown=false;
    bool display=true,retired=false,dragging=false,inCall=false,abort=false,listening=false,marked=false,rateKnown=false,slotBypassed=false,detectorMode=false;
    double rate=48000;
    struct CurveValues {
        DynamicEqSettings settings;
        std::array<float,8> movement{};
        std::array<double,421> db{};
        double rate=0;
        std::size_t selected=0;
        bool valid=false;
    };
    // Only derived GUI values are retained. Every sound/rate/movement key
    // remains exact, and geometry is rebuilt at the current component size.
    mutable std::array<CurveValues,5> curves;
    mutable PreparedDynamicEqResponse actualResponse;
    State(DynamicEqGraph& v,InstrumentProcessor& p,house::Host h,Clipboard c,std::shared_ptr<FxAnalysisHub> shared):view(v),processor(p),host(std::move(h)),clipboard(std::move(c)),monitor(p.fxMonitorHandle()),hub(shared?shared:std::make_shared<FxAnalysisHub>(monitor)),privateHub(!shared) {
        // Reuse the INIT display's actual crease, without stretching its corners
        // or copying its dot texture across the frequency plot.
        if(const auto* model=studioSkin()){
            const auto plate=model->plates.find("session");const auto display=model->displays.find("session");
            if(plate!=model->plates.end()&&display!=model->displays.end())displayRim=plate->second.getClippedImage(display->second.lattice.glass.toNearestInt().expanded(3));
        }
        if(!clipboard.read)clipboard.read=[]{return juce::SystemClipboard::getTextFromClipboard();};
        if(!clipboard.write)clipboard.write=[](const auto& text){juce::SystemClipboard::copyTextToClipboard(text);};
        int order=6;
        for(auto* button:std::array<juce::Button*,3>{&grab,&listen,&more}){button->setLookAndFeel(&keyLook);view.addAndMakeVisible(button);button->setExplicitFocusOrder(order++);button->setComponentID("eq."+button->getButtonText().toLowerCase());}
        listen.setTitle("Hold to audition selected frequency region");listen.setTooltip("Hold to audition the selected band.");
        more.setTitle("Band controls and display menu");more.setTooltip("Numeric controls, band actions, spectrum, clipboard and Sketch (S).");more.onClick=[this]{menu(true);};
        grab.setTitle("Freeze spectrum peaks and create a bell");grab.setTooltip("Freeze input peaks. Drag a handle up or down; release creates one bell. Arrow keys select and adjust; Enter creates, Escape cancels.");
        grab.onClick=[this]{view.setGrabEnabled(!grabbing);};
        listen.onStateChange=[this]{audition(listen.isDown());};
        int compareOrder=1;
        for(auto* button:std::array<juce::Button*,4>{&compareA,&compareB,&compareCopy,&autoGain}){button->setLookAndFeel(&keyLook);view.addAndMakeVisible(button);button->setExplicitFocusOrder(compareOrder++);}
        compareA.setComponentID("eq.compare.a");compareB.setComponentID("eq.compare.b");compareCopy.setComponentID("eq.compare.copy");autoGain.setComponentID("eq.auto_gain");
        compareA.setTitle("EQ comparison A");compareB.setTitle("EQ comparison B");compareCopy.setTitle("Copy current EQ to the other comparison bank");
        compareA.setTooltip("Switch to A, saving current EQ settings in the outgoing bank. Transient settings clear when the rack is replaced or sound recalled.");compareB.setTooltip("Switch to B, saving current EQ settings in the outgoing bank. Transient settings clear when the rack is replaced or sound recalled.");
        compareCopy.setTooltip("Copy current EQ into the other transient comparison bank.");autoGain.setTitle("Auto Gain estimate");autoGain.setTooltip("Estimate compensation from the EQ settings. This does not measure loudness.");
        compareA.onClick=[this]{switchComparison(false);};compareB.onClick=[this]{switchComparison(true);};compareCopy.onClick=[this]{copyComparison();};
        autoGain.onClick=[this]{toggleAutoGain();};
        gainScale.setComponentID("eq.gain_scale");gainScale.setTitle("Overall EQ gain scale");gainScale.setTooltip("Scale static bell, shelf and tilt gains. Dynamic range and output trim retain their settings.");
        gainScale.setLookAndFeel(&keyLook);gainScale.setExplicitFocusOrder(5);gainScale.setSliderStyle(juce::Slider::LinearHorizontal);gainScale.setTextBoxStyle(juce::Slider::TextBoxRight,false,58,24);gainScale.setRange(0,2,.01);gainScale.setNumDecimalPlacesToDisplay(2);gainScale.setTextValueSuffix(" x");gainScale.setValue(1,juce::dontSendNotification);
        gainScale.setColour(juce::Slider::trackColourId,curve);gainScale.setColour(juce::Slider::backgroundColourId,grid);gainScale.setColour(juce::Slider::textBoxTextColourId,paper);gainScale.setColour(juce::Slider::textBoxBackgroundColourId,juce::Colours::transparentBlack);gainScale.setColour(juce::Slider::textBoxOutlineColourId,juce::Colours::transparentBlack);
        gainScale.onDragStart=[this]{const auto keep=shared_from_this();const auto scope=frame();if(!slot||retired||closing())return;const ParameterGestureValue edit{gainField(1),processor.control(gainField(1)).getValue()};(void)beginGroup(capture(),std::span(&edit,1));};
        gainScale.onValueChange=[this]{scaleChanged();};gainScale.onDragEnd=[this]{end();};view.addAndMakeVisible(gainScale);
        scaleLabel.setText("Scale",juce::dontSendNotification);scaleLabel.setFont(juce::Font(juce::FontOptions(14)));scaleLabel.setColour(juce::Label::textColourId,paper);
        compensationLabel.setFont(juce::Font(juce::FontOptions(12)));compensationLabel.setColour(juce::Label::textColourId,paper.withAlpha(.65f));compensationLabel.setTitle("Estimated gain compensation");
        view.addAndMakeVisible(scaleLabel);view.addAndMakeVisible(compensationLabel);
        sketchApply.setLookAndFeel(&keyLook);sketchCancel.setLookAndFeel(&keyLook);
        sketchApply.setComponentID("eq.sketch.apply");sketchCancel.setComponentID("eq.sketch.cancel");
        sketchApply.setExplicitFocusOrder(1);sketchCancel.setExplicitFocusOrder(2);
        sketchApply.setTooltip("Apply the fitted static EQ using unused bands. One undo restores the previous sound.");
        sketchCancel.setTooltip("Cancel the preview without changing sound. Escape also cancels.");
        sketchApply.onClick=[this]{(void)view.applySketch();};sketchCancel.onClick=[this]{view.cancelSketch();};
        view.addChildComponent(sketchApply);view.addChildComponent(sketchCancel);
    }
    bool readRate(){const auto actual=monitor->sampleRate();const auto next=actual>0?actual:48000.;const bool changed=next!=rate||rateKnown!=(actual>0);rate=next;rateKnown=actual>0;return changed;}
    std::size_t field(std::size_t band,std::size_t offset) const {return dynamicEqParameterStart+std::size_t(*slot)*dynamicEqParametersPerSlot+band*dynamicEqParametersPerBand+offset;}
    std::size_t gainField(std::size_t offset) const {return dynamicEqGainParameterStart+std::size_t(*slot)*dynamicEqGainParametersPerSlot+offset;}
    void invalidateComparison(){comparison.clear();comparisonBankB=false;++comparisonGeneration;}
    void clearGrab(juce::String reason={}){
        grabbing=grabDragging=grabPreview=false;frozen.reset();grabDocument.reset();grabPeaks={};grabSelected=0;grabGain=0;grabStatus=std::move(reason);
    }
    std::vector<std::size_t> bandIndices(std::size_t band) const {
        if(!slot||band>=8)return {};
        auto indices=unitReach(NativeModule::dynamicEq,*slot);
        const auto prefix="effect."+juce::String(*slot+1)+".dynamic_eq.band."+juce::String(int(band+1))+".";
        std::erase_if(indices,[&](const auto index){return !parameterRegistry()[index].id.startsWith(prefix);});return indices;
    }
    std::vector<ParameterGestureValue> bandDefaults(std::size_t band) const {
        std::vector<ParameterGestureValue> edits;for(const auto index:bandIndices(band))edits.push_back({index,processor.control(index).getDefaultValue()});return edits;
    }
    std::optional<std::size_t> grabDestination(const SoundDocument& document,juce::String& reason) const{
        if(!slot||document.layout.effects[*slot].use!=SlotUse::present||document.layout.effects[*slot].kind!=EffectKind::dynamicEq){reason="Rack unavailable";return {};}
        if(document.effectLocks[*slot]){reason="Rack is locked";return {};}
        for(std::size_t band=0;band<8;++band)if(document.values[field(band,0)]<.5f){
            for(const auto index:bandIndices(band)){
                if(document.controlLocks[index]){reason="Next unused band is locked";return {};}
                for(const auto& macro:document.macros)if(macro.targetKey==parameterRegistry()[index].stateKey){reason="Next unused band has a macro assignment";return {};}
            }
            return band;
        }
        reason="All eight bands are active";return {};
    }
    bool grabContextCurrent() const{
        if(!slot||!token||!frozen||!grabDocument||hub->generation(*token)!=frozen->generation
            ||monitor->stats(*slot).dropped!=grabDropped||monitor->sampleRate()!=frozen->sampleRate)return false;
        SoundDocument current;std::uint64_t revision,reset;
        return processor.trySoundSnapshot(current,revision,reset)&&revision==grabRevision&&reset==grabReset&&current==*grabDocument;
    }
    void freezeGrab(){
        if(!grabbing||frozen||!slot||!token)return;
        const auto& captured=analysisResult();
        if(!captured.valid||hub->generation(*token)!=captured.generation)return;
        auto document=std::make_shared<SoundDocument>();std::uint64_t revision,reset;juce::String reason;
        if(!processor.trySoundSnapshot(*document,revision,reset)||!validateSoundDocument(*document)
            ||document->layout!=sound.layout||rackOwner(*document,*slot)!=rackOwner(sound,*slot)){clearGrab("Sound changed; grab again");return;}
        if(!grabDestination(*document,reason)){clearGrab(reason);return;}
        frozen=std::make_unique<FxSpectrum::Result>(captured);grabPeaks=findSpectrumPeaks(*frozen);
        grabDocument=std::move(document);grabRevision=revision;grabReset=reset;grabDropped=monitor->stats(*slot).dropped;
        grabStatus=grabPeaks.size?"Drag a peak; release creates a bell. Arrows adjust, Enter creates, Escape cancels.":"No distinct peaks; toggle GRAB to try again";
    }
    void commitGrab(){
        const auto keep=shared_from_this();const auto scope=frame();
        if(retired||closing()||!grabbing)return;
        if(!grabContextCurrent()||grabSelected>=grabPeaks.size){clearGrab("Sound or spectrum changed; grab again");sync();view.repaint();return;}
        juce::String reason;const auto destination=grabDestination(*grabDocument,reason);
        if(!destination){clearGrab(reason);sync();view.repaint();return;}
        const auto expected=grabDocument;const auto revision=grabRevision,reset=grabReset;
        const auto peak=grabPeaks.values[grabSelected];const auto gain=grabGain;const auto band=*destination,a=field(band,0);
        auto edits=bandDefaults(band);
        for(auto& edit:edits){if(edit.index==a)edit.value=1;
            else if(edit.index==field(band,3))edit.value=processor.control(edit.index).convertTo0to1(float(peak.frequencyHz));
            else if(edit.index==field(band,4))edit.value=processor.control(edit.index).convertTo0to1(gain);
            else if(edit.index==field(band,5))edit.value=processor.control(edit.index).convertTo0to1(peak.q);}
        clearGrab();sync();view.repaint();
        if(!begin(a))return;
        inCall=true;const bool ok=processor.setParameterGestureValues(a,edits,*expected,revision,reset);inCall=false;
        if(!ok)report();end();
        if(!retired&&!closing()){SoundDocument current;std::uint64_t edit;if(processor.trySoundSnapshot(current,edit)&&slot)view.refresh(current,*slot);if(ok)view.selectBand(band);changed();}
    }
    std::shared_ptr<void> frame() const{return host.frame?host.frame():nullptr;}
    bool closing() const{return host.closing&&host.closing();}
    void report(){if(!retired&&!closing()&&host.report)host.report(rejectedEdit);}
    void changed(){if(!retired&&!closing()){const auto notify=view.onEdited;if(notify)notify();}}
    void end(){
        const auto keep=shared_from_this();
        if(inCall){abort=true;return;}
        groupGesture=false;groupExpected.reset();
        if(!dragging)return;
        const auto scope=frame();const auto index=leader;dragging=false;abort=false;inCall=true;
        const bool ok=processor.endParameterGesture(index);inCall=false;if(!ok)report();
    }
    bool begin(std::size_t index){
        const auto keep=shared_from_this();
        const auto scope=frame();if(retired||closing()||!slot)return false;end();if(retired||closing()||!slot)return false;leader=index;abort=false;
        inCall=true;const bool ok=processor.beginParameterGesture(index);inCall=false;dragging=ok;
        if(retired||closing()||abort){end();return false;}if(!ok)report();return ok;
    }
    void write(std::span<const ParameterGestureValue> edits){
        const auto keep=shared_from_this();
        const auto scope=frame();if(!dragging||retired||closing()){end();return;}
        inCall=true;const bool ok=processor.setParameterGestureValues(leader,edits);inCall=false;
        if(!ok||retired||closing()||abort)end();
        if(!ok)report();
        if(!retired&&!closing()){SoundDocument d;std::uint64_t revision;if(processor.trySoundSnapshot(d,revision)&&slot)view.refresh(d,*slot);changed();}
    }
    void single(std::size_t index,float normalized){
        const auto keep=shared_from_this();const auto scope=frame();
        if(retired||closing()||!slot)return;
        if(begin(index)){const ParameterGestureValue edit{index,normalized};write(std::span(&edit,1));end();}
    }
    std::bitset<8> editBands() const {if(selection.any())return selection;std::bitset<8> one;one.set(selected);return one;}
    std::shared_ptr<const EditContext> capture(bool forSketch=false) {
        const auto keep=shared_from_this();const auto scope=frame();
        if(retired||closing()||!slot||grabbing||(sketching&&!forSketch))return {};
        auto context=std::make_shared<EditContext>();context->slot=*slot;context->binding=bindingGeneration;
        if(!processor.trySoundSnapshot(context->document,context->revision,context->reset))return {};
        if(resetKnown&&context->reset!=observedReset){observedReset=context->reset;invalidateComparison();clearSelection();if(!retired&&!closing()){sync();view.repaint();}return {};}
        if(context->document!=sound)return {};
        return context;
    }
    bool current(const EditContext& context) const {
        if(retired||closing()||!slot||grabbing||*slot!=context.slot||bindingGeneration!=context.binding)return false;
        SoundDocument live;std::uint64_t revision,reset;
        return processor.trySoundSnapshot(live,revision,reset)&&revision==context.revision&&reset==context.reset&&live==context.document;
    }
    bool beginGroup(const std::shared_ptr<const EditContext>& context,std::span<const ParameterGestureValue> edits){
        const auto keep=shared_from_this();const auto scope=frame();
        if(!context||edits.empty()||!current(*context))return false;
        end();if(retired||closing()||!slot)return false;
        leader=edits.front().index;abort=false;groupExpected=context;groupGesture=true;editStatus.clear();
        inCall=true;const bool ok=processor.beginParameterGesture(leader,edits,context->document,context->revision,context->reset);inCall=false;dragging=ok;
        if(!ok||retired||closing()||abort){end();if(!ok)report();return false;}return true;
    }
    bool writeGroup(std::span<const ParameterGestureValue> edits){
        const auto keep=shared_from_this();const auto scope=frame();const auto context=groupExpected;
        if(!context||!dragging||!groupGesture||retired||closing()){end();return false;}
        inCall=true;const bool ok=processor.setParameterGestureValues(leader,edits,context->document,context->revision,context->reset);inCall=false;
        if(!ok||retired||closing()||abort){end();if(!ok)report();return false;}
        auto next=std::make_shared<EditContext>();next->slot=context->slot;next->binding=context->binding;
        if(!processor.trySoundSnapshot(next->document,next->revision,next->reset)||next->reset!=context->reset
            ||!slot||*slot!=context->slot||bindingGeneration!=context->binding){end();return false;}
        groupExpected=next;view.refresh(next->document,next->slot);changed();return !retired&&!closing()&&!abort;
    }
    bool execute(const std::shared_ptr<const EditContext>& context,std::span<const ParameterGestureValue> edits){
        const auto keep=shared_from_this();const auto scope=frame();if(!beginGroup(context,edits))return false;
        const bool ok=writeGroup(edits);end();return ok;
    }
    std::optional<DynamicEqSettings> rawEq(const EditContext& context) const {
        auto raw=context.document;raw.macros={};RoutedPatch patch;
        if(!makeRoutedPatch(raw,patch))return {};
        return patch.effects[context.slot].dynamicEq;
    }
    std::bitset<8> sketchDestinations(const EditContext& context) const {
        std::bitset<8> available;
        const auto& document=context.document;
        if(document.effectLocks[context.slot])return available;
        const auto indices=unitReach(NativeModule::dynamicEq,context.slot);
        for(std::size_t band=0;band<8;++band){
            const auto active=dynamicEqParameterStart+context.slot*dynamicEqParametersPerSlot+band*dynamicEqParametersPerBand;
            if(document.values[active]>=.5f)continue;
            const auto prefix="effect."+juce::String(context.slot+1)+".dynamic_eq.band."+juce::String(int(band+1))+".";
            bool free=true;
            for(const auto index:indices)if(parameterRegistry()[index].id.startsWith(prefix)){
                if(document.controlLocks[index])free=false;
                for(const auto& macro:document.macros)if(macro.targetKey==parameterRegistry()[index].stateKey)free=false;
            }
            available.set(band,free);
        }
        return available;
    }
    void clearSketch(juce::String reason={}) {
        const bool wasMatch=matching;
        sketchJob.reset();sketchContext.reset();sketchStrokeContext.reset();
        sketchTarget.reset();sketchFit.reset();sketchError.reset();sketchAdded.reset();
        matchBase.reset();matchFit.reset();matching=false;
        sketchPointsSize=0;sketching=sketchDrawing=false;sketchStatus.clear();
        if(reason.isNotEmpty())editStatus=(wasMatch?"Match: ":"Sketch: ")+reason;
    }
    bool queueSketch(const EqCurve& target,const std::shared_ptr<const EditContext>& context) {
        if(!sketching||candidate||!context||!current(*context))return false;
        if(readRate()){clearSketch("Sample rate changed; draw again");sync();view.repaint();return false;}
        for(const auto value:target.db)if(!std::isfinite(value))return false;
        auto source=rawEq(*context);const auto available=sketchDestinations(*context);
        if(matching){RoutedPatch patch;if(!makeRoutedPatch(context->document,patch))return false;source=patch.effects[context->slot].dynamicEq;}
        if(!source||available.none()||source->gainScale==0)return false;
        // Replacing a stroke cancels and joins the sole previous control job.
        // The worker never captures this State, the view or the processor.
        sketchJob.reset();sketchContext=context;sketchFit.reset();sketchError.reset();sketchAdded.reset();
        sketchTarget=target;sketchStatus="Fitting static EQ...";
        try {
            sketchJob=std::make_unique<SketchJob>();sketchJob->context=context;
            sketchJob->target=target;sketchJob->source=*source;sketchJob->available=available;sketchJob->rate=rate;
            sketchJob->start();
        } catch(...) {sketchJob.reset();sketchStatus="Could not start the fit; draw again";sync();view.repaint();return false;}
        sync();view.repaint();return true;
    }
    void pollSketch() {
        if(!sketching)return;
        const auto keep=shared_from_this();
        if(candidate||(sketchContext&&!current(*sketchContext))||(sketchJob&&sketchJob->rate!=rate)){
            clearSketch("Sound or sample rate changed; draw again");sync();view.repaint();return;
        }
        if(!sketchJob||!sketchJob->done.load(std::memory_order_acquire))return;
        const auto result=sketchJob->result;const auto available=sketchJob->available;sketchJob.reset();
        sketchError=result.rmsDb;
        if(matching&&matchBase&&result.ready){
            PreparedDynamicEqResponse response;
            if(response.prepare(result.settings,rate)){
                EqCurve curve;bool valid=true;
                for(std::size_t i=0;i<129;++i){const auto frequency=20*std::pow(1000.,double(i)/128);if(frequency>rate*.5)continue;
                    const auto energy=response.energy(frequency);if(!energy){valid=false;break;}curve.db[i]=float(20*std::log10(std::max(*energy,1e-12)))-matchBase->db[i];}
                if(valid)matchFit=curve;
            }
        }
        if(result.ready&&sketchContext){
            const auto source=rawEq(*sketchContext);
            if(source)for(std::size_t band=0;band<8;++band)
                if(available[band]&&!source->bands[band].active&&result.settings.bands[band].active)sketchAdded.set(band);
            if(sketchAdded.any())sketchFit=result.settings;
        }
        if(sketchFit)sketchStatus=juce::String(int(sketchAdded.count()))+" new "+(sketchAdded.count()==1?"band":"bands")+"   Error "+juce::String(*sketchError,2)+" dB RMS";
        else sketchStatus=(result.ready&&result.rmsDb<.05f?juce::String("Curve already matches"):juce::String("No closer fit with the available bands"))+"   Error "+juce::String(*sketchError,2)+" dB RMS";
        sync();view.repaint();
    }
    void addSketchPoint(juce::Point<float> point) {
        const auto r=view.plotBounds();
        point={std::clamp((point.x-r.getX())/std::max(1.f,r.getWidth()),0.f,1.f),std::clamp(gainAt(r,point.y),-12.f,12.f)};
        if(sketchPointsSize==sketchPoints.size()){
            // Keep the first point and latest endpoint while thinning the
            // interior; memory remains fixed for arbitrarily long strokes.
            for(std::size_t i=1;i<128;++i)sketchPoints[i]=sketchPoints[2*i];
            sketchPointsSize=128;
        }
        sketchPoints[sketchPointsSize++]=point;
    }
    void beginSketchStroke(juce::Point<float> point) {
        sketchJob.reset();sketchFit.reset();sketchError.reset();sketchTarget.reset();sketchAdded.reset();
        sketchStrokeContext=capture(true);sketchContext=sketchStrokeContext;
        if(!sketchStrokeContext){clearSketch("Sound changed; draw again");sync();return;}
        sketchPointsSize=0;sketchDrawing=true;sketchStatus="Draw a curve; release to preview";addSketchPoint(point);
    }
    void finishSketchStroke() {
        sketchDrawing=false;
        const auto context=sketchStrokeContext;sketchStrokeContext.reset();
        if(!context||!current(*context)||sketchPointsSize<2){sketchStatus="Draw a longer curve to preview";sync();return;}
        struct Point {float x=0,db=0;std::size_t order=0;};
        std::array<Point,256> sorted{};
        for(std::size_t i=0;i<sketchPointsSize;++i)sorted[i]={sketchPoints[i].x,sketchPoints[i].y,i};
        std::sort(sorted.begin(),sorted.begin()+sketchPointsSize,[](const auto& a,const auto& b){return a.x<b.x||(a.x==b.x&&a.order<b.order);});
        std::size_t count=0;
        for(std::size_t i=0;i<sketchPointsSize;++i){if(count&&sorted[count-1].x==sorted[i].x)sorted[count-1]=sorted[i];else sorted[count++]=sorted[i];}
        if(count<2){sketchStatus="Draw across more than one frequency";sync();return;}
        const auto source=rawEq(*context);if(!source)return;
        EqCurve target;std::size_t segment=0;
        for(std::size_t i=0;i<target.db.size();++i){
            const float x=float(i)/128;const auto response=componentTransfer(*source,rate,20.*std::pow(1000.,double(i)/128.),DynamicEqPlacement::stereo);
            target.db[i]=response?float(decibels(*response)):0.f;
            if(x<sorted[0].x||x>sorted[count-1].x)continue;
            while(segment+1<count-1&&sorted[segment+1].x<x)++segment;
            const auto& a=sorted[segment];const auto& b=sorted[segment+1];
            target.db[i]=std::lerp(a.db,b.db,std::clamp((x-a.x)/(b.x-a.x),0.f,1.f));
        }
        if(!queueSketch(target,context)){sketchStatus="Sound changed or no unused bands are available";sync();}
    }
    bool applySketch() {
        const auto keep=shared_from_this();const auto scope=frame();
        if(!sketching)return false;
        if(readRate()){clearSketch("Sample rate changed; draw again");sync();view.repaint();return false;}
        pollSketch();
        const auto context=sketchContext;
        if(!sketching||!sketchFit||!context||!current(*context)||candidate)return false;
        const auto available=sketchDestinations(*context),added=sketchAdded;
        if((added&available)!=added)return false;
        EqPatch patch;const auto indices=unitReach(NativeModule::dynamicEq,context->slot);
        const auto slotPrefix="effect."+juce::String(context->slot+1)+".";
        for(std::uint8_t band=0;band<8;++band)if(added[band]){
            EqBandPatch addition;addition.band=band;
            const auto prefix="dynamic_eq.band."+juce::String(band+1)+".";
            for(const auto index:indices){const auto key=parameterRegistry()[index].id.substring(slotPrefix.length());
                if(!key.startsWith(prefix))continue;
                auto normalized=processor.control(index).getDefaultValue();const auto& b=sketchFit->bands[band];
                if(key==prefix+"active")normalized=1;
                if(key==prefix+"shape")normalized=processor.control(index).convertTo0to1(float(b.shape));
                if(key==prefix+"frequency_hz")normalized=processor.control(index).convertTo0to1(b.frequencyHz);
                if(key==prefix+"gain_db")normalized=processor.control(index).convertTo0to1(b.gainDb);
                if(key==prefix+"q")normalized=processor.control(index).convertTo0to1(b.q);
                addition.fields.push_back({key,normalized});
            }
            patch.bands.push_back(std::move(addition));
        }
        SoundDocument candidateSound;if(!makeEqEdit(context->document,context->slot,patch,candidateSound))return false;
        std::vector<ParameterGestureValue> edits;
        for(const auto index:indices)if(candidateSound.values[index]!=context->document.values[index])edits.push_back({index,candidateSound.values[index]});
        if(edits.empty())return false;
        clearSketch();sketchApplying=true;const bool ok=execute(context,edits);sketchApplying=false;
        if(!retired&&!closing()){if(ok)view.setSelectedBands(added);sync();view.repaint();}
        return ok;
    }
    bool comparisonRefused(const juce::String& reason){
        if(!retired&&!closing()){editStatus="Comparison refused: "+reason;sync();view.repaint();}return false;
    }
    bool storeComparison(bool bankB){
        const auto keep=shared_from_this();const auto scope=frame();const auto context=capture();
        if(!context||!current(*context))return comparisonRefused("sound changed");
        std::vector<EqBandPatch> captured;captured.reserve(8);
        for(std::uint8_t band=0;band<8;++band){EqBandPatch raw;if(!captureEqBand(context->document,context->slot,band,raw))return false;captured.push_back(std::move(raw));}
        std::vector<EqField> fields;const auto prefix="effect."+juce::String(context->slot+1)+".";
        for(const auto index:unitReach(NativeModule::dynamicEq,context->slot)){
            const auto key=parameterRegistry()[index].id.substring(prefix.length());
            if(!key.startsWith("dynamic_eq.band."))fields.push_back({key,context->document.values[index]});
        }
        return current(*context)&&comparison.store(bankB,captured,fields);
    }
    bool recallComparison(bool bankB){
        const auto keep=shared_from_this();const auto scope=frame();const auto context=capture();
        if(!context||!current(*context))return comparisonRefused("sound changed");
        const auto saved=comparison.recall(bankB);if(!saved)return comparisonRefused("bank is empty");
        SoundDocument candidate;if(!makeEqEdit(context->document,context->slot,*saved,candidate))return comparisonRefused("rack or controls are locked or unavailable");
        std::vector<ParameterGestureValue> edits;
        for(const auto index:unitReach(NativeModule::dynamicEq,context->slot))
            if(candidate.values[index]!=context->document.values[index])edits.push_back({index,candidate.values[index]});
        const auto generation=comparisonGeneration;
        if(!edits.empty()&&!execute(context,edits))return comparisonRefused("sound changed or controls are unavailable");
        if(retired||closing()||comparisonGeneration!=generation||!slot||*slot!=context->slot)return false;
        SoundDocument live;std::uint64_t revision,reset;
        if(!processor.trySoundSnapshot(live,revision,reset)||reset!=context->reset){invalidateComparison();return comparisonRefused("sound was recalled");}
        comparisonBankB=bankB;editStatus.clear();sync();view.repaint();return true;
    }
    void switchComparison(bool bankB){
        const auto keep=shared_from_this();const auto scope=frame();if(retired||closing()||!slot)return;
        if(bankB==comparisonBankB&&comparison.recall(bankB))return;
        if(!comparison.recall(bankB)&&!storeComparison(bankB))return;
        if(!storeComparison(comparisonBankB))return;
        (void)recallComparison(bankB);
    }
    void copyComparison(){
        const auto keep=shared_from_this();const auto scope=frame();if(retired||closing()||!slot)return;
        if(storeComparison(!comparisonBankB)&&!retired&&!closing()){editStatus.clear();sync();view.repaint();}
    }
    void scaleChanged(){
        const auto keep=shared_from_this();const auto scope=frame();if(retired||closing()||!slot)return;
        const auto index=gainField(1);const ParameterGestureValue edit{index,processor.control(index).convertTo0to1(float(gainScale.getValue()))};
        if(dragging&&groupGesture&&leader==index)(void)writeGroup(std::span(&edit,1));
        else (void)execute(capture(),std::span(&edit,1));
        if(!retired&&!closing()){sync();view.repaint();}
    }
    void toggleAutoGain(){
        const auto keep=shared_from_this();const auto scope=frame();if(retired||closing()||!slot)return;
        const ParameterGestureValue edit{gainField(0),base.autoGain?0.f:1.f};
        (void)execute(capture(),std::span(&edit,1));
        if(!retired&&!closing()){sync();view.repaint();}
    }
    void clearSelection(){selection.reset();marquee=false;editStatus.clear();++bindingGeneration;end();}
    void setBandFields(std::size_t offset,float value){
        const auto context=capture();if(!context)return;std::vector<ParameterGestureValue> edits;
        const auto targets=editBands();for(std::size_t band=0;band<8;++band)if(targets[band])edits.push_back({field(band,offset),value});
        (void)execute(context,edits);
    }
    void toggle(std::size_t offset){if(slot&&!retired&&!closing())setBandFields(offset,processor.control(field(selected,offset)).getValue()>=.5f?0.f:1.f);}
    bool copy(const EditContext& context,std::bitset<8> selectedBands,juce::String& out) const {
        if(selectedBands.none()||!current(context))return false;std::vector<EqBandPatch> bands;
        for(std::uint8_t band=0;band<8;++band)if(selectedBands[band]){EqBandPatch patch;if(!captureEqBand(context.document,context.slot,band,patch))return false;bands.push_back(std::move(patch));}
        return encodeEqClipboard(bands,out);
    }
    bool paste(const std::shared_ptr<const EditContext>& context,const juce::String& text){
        const auto fail=[&](const juce::String& reason){if(!retired&&!closing()){editStatus="Paste refused: "+reason;sync();view.repaint();}return false;};
        if(!context||!current(*context))return fail("sound changed");std::vector<EqBandPatch> copied;EqPatch patch;
        if(!decodeEqClipboard(text,copied))return fail("invalid band data");
        if(context->document.effectLocks[context->slot])return fail("rack is locked");
        std::size_t free=0;for(std::size_t n=0;n<8;++n)if(context->document.values[field(n,0)]<.5f)++free;
        if(free<copied.size())return fail(juce::String(int(copied.size()))+" unused bands needed; "+juce::String(int(free))+" available");
        if(!makeEqPaste(context->document,context->slot,copied,patch))return fail("destination bands are locked or unavailable");
        std::vector<ParameterGestureValue> edits;std::bitset<8> pasted;
        for(const auto& band:patch.bands){pasted.set(band.band);for(const auto& value:band.fields){const auto index=rackParameterIndex(NativeModule::dynamicEq,context->slot,value.key);if(!index)return false;edits.push_back({*index,value.normalized});}}
        if(!execute(context,edits))return fail("sound changed or controls are unavailable");
        if(!retired&&!closing()&&slot&&*slot==context->slot&&bindingGeneration==context->binding){view.setSelectedBands(pasted);changed();}return true;
    }
    void clipboardCommand(bool isPaste,const std::shared_ptr<const EditContext>& context,std::bitset<8> copiedSelection){
        const auto keep=shared_from_this();const auto scope=frame();if(!context||!current(*context))return;
        // Clipboard providers and async popup callbacks may reenter the editor.
        if(isPaste){const auto text=clipboard.read();if(!retired&&!closing()&&!paste(context,text))report();}
        else {juce::String text;if(copy(*context,copiedSelection,text))clipboard.write(text);}
    }
    void menuCommand(int item,const std::shared_ptr<const EditContext>& context,std::bitset<8> copiedSelection,std::size_t primary){
        const auto keep=shared_from_this();const auto scope=frame();if(!context||!current(*context)||retired||closing())return;
        if(item==1||item==2){clipboardCommand(item==2,context,copiedSelection);return;}
        if(item==0)return;
        selection=copiedSelection;selected=primary;
        switch(item){
            case 3:view.selectBand(selected);break;
            case 4:toggle(0);break;
            case 5:toggle(1);break;
            case 6:addAt(view.plotBounds().getCentre());break;
            case 7:setBandFields(0,0);break;
            case 8:view.setSpectrumOn(!display);break;
            case 9:view.setSketchEnabled(true);break;
            case 10:if(view.onOverview)view.onOverview();break;
            case 11:if(view.onMatch)view.onMatch();break;
            case 12:if(view.onSpectral)view.onSpectral();break;
            default:if(item>=20&&item<28)view.selectBand(std::size_t(item-20));break;
        }
        if(!retired&&!closing()){sync();view.repaint();}
    }
    void menu(bool toolbar=false){
        const auto keep=shared_from_this();const auto scope=frame();
        const auto context=capture();if(!context)return;const auto copiedSelection=selection;const auto primary=selected;
        juce::PopupMenu menu;menu.addItem(1,"Copy selected bands\tCtrl+C",selection.any());menu.addItem(2,"Paste bands\tCtrl+V");
        menu.addSeparator();menu.addItem(3,"Band "+juce::String(int(selected+1))+" numeric controls");
        menu.addItem(4,"Band active",true,base.bands[selected].active);menu.addItem(5,"Bypass selected bands",true,base.bands[selected].bypassed);
        menu.addItem(6,"Add band",std::any_of(base.bands.begin(),base.bands.end(),[](const auto& b){return !b.active;}));
        const auto targets=editBands();bool anyActive=false;for(std::size_t n=0;n<8;++n)anyActive=anyActive||(targets[n]&&base.bands[n].active);
        menu.addItem(7,"Remove selected bands\tDelete",anyActive);menu.addSeparator();menu.addItem(8,"Input / output spectrum",true,display);
        juce::PopupMenu bandMenu;for(std::size_t n=0;n<8;++n)bandMenu.addItem(int(20+n),"Band "+juce::String(int(n+1))+(base.bands[n].active?(base.bands[n].bypassed?" (bypassed)":""):" (unused)"),true,n==selected);
        menu.addSubMenu("Select band",bandMenu);
        menu.addSeparator();menu.addItem(9,"Sketch EQ curve\tS",!candidate&&sketchDestinations(*context).any());
        menu.addItem(10,"Spectrum overview",bool(view.onOverview));
        menu.addItem(11,"EQ Match",bool(view.onMatch));
        menu.addItem(12,"Spectral dynamics",bool(view.onSpectral));
        const std::weak_ptr<State> weak=keep;const auto generation=++menuGeneration;menuOpen=true;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(toolbar?static_cast<juce::Component*>(&more):&view).withInitiallySelectedItem(toolbar?3:selection.any()?1:2),[weak,context,copiedSelection,primary,generation](int item){
            if(const auto owner=weak.lock();owner&&!owner->retired&&owner->menuGeneration==generation){owner->menuOpen=false;owner->menuCommand(item,context,copiedSelection,primary);}
        });
    }
    void addAt(juce::Point<float> at){
        const auto keep=shared_from_this();const auto scope=frame();const auto context=capture();if(!context)return;
        juce::String reason;const auto destination=grabDestination(context->document,reason);
        if(!destination){editStatus="Add band: "+reason;sync();view.repaint();return;}
        const auto n=*destination;auto edits=bandDefaults(n);
        for(auto& edit:edits){if(edit.index==field(n,0))edit.value=1;
            else if(edit.index==field(n,3))edit.value=processor.control(edit.index).convertTo0to1(float(frequencyAt(view.plotBounds(),at.x)));
            else if(edit.index==field(n,4))edit.value=processor.control(edit.index).convertTo0to1(gainAt(view.plotBounds(),at.y));}
        if(execute(context,edits)&&!retired&&!closing())view.selectBand(n);
    }
    bool visible() const {
        if(!view.isShowing()||view.getWidth()<=0||view.getHeight()<=0)return false;
        auto area=view.getLocalBounds();const juce::Component* child=&view;
        for(auto* parent=view.getParentComponent();parent;parent=parent->getParentComponent()){
            area=parent->getLocalArea(child,area).getIntersection(parent->getLocalBounds());if(area.isEmpty())return false;child=parent;
        }
        return true;
    }
    void unsubscribe(){
        listening=false;listen.setToggleState(false,juce::dontSendNotification);if(token){(void)hub->requestAudition(*token,std::nullopt);hub->release(*token);token.reset();}
    }
    bool detectorAvailable() const {
        const auto& b=settings.bands[selected];
        const bool external=b.detectorSource==DynamicEqDetectorSource::externalBand||b.detectorSource==DynamicEqDetectorSource::externalFree;
        return hasDynamics(b.shape)&&!b.bypassed&&(!external||monitor->externalAvailable());
    }
    void audition(bool down){
        if(retired||!slot)return;
        if(down)clearGrab();
        if(down&&(slotBypassed||!base.bands[selected].active||!visible()||(detectorMode&&!detectorAvailable())))return;
        listening=down;
        if(down&&!token)token=hub->acquire(*slot);
        if(token && !hub->requestAudition(*token,down?std::optional<std::size_t>(selected):std::nullopt,detectorMode))listening=false;
        if(!display&&!listening&&!detectorMode)unsubscribe();
        sync();view.repaint();
    }
    void sync(){
        for(auto* button:std::array<juce::Button*,7>{&grab,&listen,&more,&compareA,&compareB,&compareCopy,&autoGain})button->setVisible(!sketching);
        gainScale.setVisible(!sketching);scaleLabel.setVisible(!sketching);compensationLabel.setVisible(!sketching);
        sketchApply.setVisible(sketching);sketchCancel.setVisible(sketching);sketchApply.setEnabled(sketchFit.has_value());
        if(!slot)return;
        const auto address="FX "+juce::String(int(*slot+1))+", band "+juce::String(int(selected+1));
        view.setTitle("FX "+juce::String(int(*slot+1))+" Dynamic EQ graph");
        view.setDescription(juce::String(slotBypassed?"Slot bypassed; showing retained EQ. ":"")+"Shift-click toggles bands; drag empty space to select. Drag or arrows move selected bands together, Shift for fine arrow steps; wheel Q. Ctrl+C copies raw bands, Ctrl+V pastes into unused bands. More or right-click opens band controls, selection and clipboard. Hold Listen to audition the primary band. "+grabStatus+" "+editStatus);
        if(sketching){view.setTitle("FX "+juce::String(int(*slot+1))+" Sketch EQ preview");view.setDescription(juce::String(hasComponentBands(settings)?"Draw a stereo-energy response; new bands use Stereo. ":"Draw a static response. ")+"Target is dashed amber; fit is green. Apply uses unused bands with one undo. Escape or Cancel discards the preview. "+sketchStatus);}
        listen.setButtonText(detectorMode?"Trigger":"Listen");listen.setTitle("Hold to audition "+address+(detectorMode?" detector trigger":" frequency region"));
        listen.setTooltip(detectorMode?"Hold to hear the selected detector source and filter. External input must be connected.":"Hold to hear the selected band's frequency region.");
        listen.setEnabled(base.bands[selected].active&&!slotBypassed&&(!detectorMode||detectorAvailable()));listen.setToggleState(listening,juce::dontSendNotification);more.setEnabled(!grabbing);
        juce::String reason;const bool available=grabDestination(sound,reason).has_value();grab.setEnabled(display&&!slotBypassed&&!proposal&&(available||grabbing));grab.setToggleState(grabbing,juce::dontSendNotification);
        if(!available)grab.setTooltip(reason);else grab.setTooltip("Freeze input peaks. Drag vertically to set gain, Enter creates, Escape cancels. The preview does not change sound.");
        compareA.setToggleState(!comparisonBankB,juce::dontSendNotification);compareB.setToggleState(comparisonBankB,juce::dontSendNotification);compareCopy.setButtonText(comparisonBankB?"Copy B > A":"Copy A > B");
        compareA.setEnabled(!grabbing);compareB.setEnabled(!grabbing);compareCopy.setEnabled(!grabbing);
        autoGain.setToggleState(base.autoGain,juce::dontSendNotification);autoGain.setEnabled(!grabbing&&!sound.effectLocks[*slot]&&!sound.controlLocks[gainField(0)]);
        gainScale.setEnabled(!grabbing&&!sound.effectLocks[*slot]&&!sound.controlLocks[gainField(1)]);gainScale.setValue(base.gainScale,juce::dontSendNotification);
        const auto estimate=dynamicEqAutoGainDb(settings);compensationLabel.setText(base.autoGain?"est "+juce::String(estimate>0?"+":"")+juce::String(estimate==0?0.f:estimate,1)+" dB":"",juce::dontSendNotification);
    }
    void updateProposal(){
        proposal.reset();marked=false;if(!candidate||!slot||sound.effectLocks[*slot])return;
        auto admitted=std::make_unique<SoundDocument>(*candidate);
        for(std::size_t i=0;i<parameterCount;++i)if(sound.controlLocks[i])admitted->values[i]=sound.values[i];
        RoutedPatch patch;if(!makeRoutedPatch(*admitted,patch)||patch.layout.effects[*slot].use!=SlotUse::present||patch.layout.effects[*slot].kind!=EffectKind::dynamicEq)return;
        for(const auto i:unitReach(NativeModule::dynamicEq,*slot))marked=marked||std::abs(admitted->values[i]-sound.values[i])>1e-6f;
        if(marked)proposal=patch.effects[*slot].dynamicEq;
    }
    std::vector<ParameterGestureValue> nodeValues(double frequencyDelta,double gainDelta) const {
        double lowest=20000,highest=20,lowGain=24,highGain=-24;bool gain=false;
        for(std::size_t n=0;n<8;++n)if(gestureBands[n]){const auto& b=groupBase.bands[n];lowest=std::min(lowest,double(b.frequencyHz));highest=std::max(highest,double(b.frequencyHz));
            if(hasGain(b.shape)){gain=true;lowGain=std::min(lowGain,double(b.gainDb));highGain=std::max(highGain,double(b.gainDb));}}
        const auto df=std::clamp(frequencyDelta,std::log(20/lowest),std::log(20000/highest));
        const auto dg=gain?std::clamp(gainDelta,-24-lowGain,24-highGain):0.;std::vector<ParameterGestureValue> edits;edits.reserve(16);
        for(std::size_t n=0;n<8;++n)if(gestureBands[n]){const auto& b=groupBase.bands[n];const auto f=field(n,3),g=field(n,4);
            edits.push_back({f,processor.control(f).convertTo0to1(float(std::clamp(b.frequencyHz*std::exp(df),20.,20000.)))});
            if(hasGain(b.shape))edits.push_back({g,processor.control(g).convertTo0to1(float(std::clamp(b.gainDb+dg,-24.,24.)))});
        }return edits;
    }
    bool beginNode(juce::Point<float> at){
        const auto context=capture();if(!context)return false;groupBase=base;gestureBands=editBands();groupPointer=at;
        return beginGroup(context,nodeValues(0,0));
    }
    void nodeEdit(juce::Point<float> at){
        if(!slot||!dragging||!groupGesture)return;const auto r=view.plotBounds();
        const auto edits=nodeValues(logSpan*(at.x-groupPointer.x)/std::max(1.f,r.getWidth()),(groupPointer.y-at.y)*60/std::max(1.f,r.getHeight()));
        (void)writeGroup(edits);
    }
    void wheelQ(float delta){
        const auto context=capture();if(!context)return;const auto targets=editBands();double lowest=18,highest=.25;bool any=false;
        for(std::size_t n=0;n<8;++n)if(targets[n]&&hasQ(base.bands[n].shape)){any=true;lowest=std::min(lowest,double(base.bands[n].q));highest=std::max(highest,double(base.bands[n].q));}
        if(!any)return;const auto factor=std::exp(std::clamp(double(delta),std::log(.25/lowest),std::log(18/highest)));std::vector<ParameterGestureValue> edits;
        for(std::size_t n=0;n<8;++n)if(targets[n]&&hasQ(base.bands[n].shape)){const auto q=field(n,5);edits.push_back({q,processor.control(q).convertTo0to1(float(std::clamp(base.bands[n].q*factor,.25,18.)))});}
        (void)execute(context,edits);
    }
};
DynamicEqGraph::DynamicEqGraph(InstrumentProcessor& p,house::Host h,Clipboard c,std::shared_ptr<FxAnalysisHub> hub):state(std::make_shared<State>(*this,p,std::move(h),std::move(c),std::move(hub))){
    setComponentID("dynamic.eq.graph");setTitle("Dynamic EQ frequency graph");setDescription("Eight fixed bands. Drag frequency and gain; wheel Q. Arrow keys move the selected band's stored controls; Shift gives fine steps. More opens numeric controls and band actions. Hold Listen to audition.");setWantsKeyboardFocus(true);setExplicitFocusOrder(9);if(state->privateHub)startTimerHz(30);
}
DynamicEqGraph::~DynamicEqGraph(){retire();}
void DynamicEqGraph::refresh(const SoundDocument& d,std::uint8_t slot){
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing())return;
    SoundDocument live;std::uint64_t revision,reset;
    if(s.processor.trySoundSnapshot(live,revision,reset)){const bool recalled=s.resetKnown&&s.observedReset!=reset;s.observedReset=reset;s.resetKnown=true;if(recalled){s.invalidateComparison();s.clearGrab();s.clearSketch();s.clearSelection();}}
    if(s.retired||s.closing())return;
    if(slot>=8||d.layout.effects[slot].use!=SlotUse::present||d.layout.effects[slot].kind!=EffectKind::dynamicEq){s.invalidateComparison();suspend();if(s.retired)return;s.slot.reset();setEnabled(false);return;}
    if(s.slot&&(*s.slot!=slot||rackOwner(s.sound,*s.slot)!=rackOwner(d,slot)))s.invalidateComparison();
    if(s.slot&&(*s.slot!=slot||s.sound.layout!=d.layout||rackOwner(s.sound,*s.slot)!=rackOwner(d,slot)))suspend();
    if(s.grabbing&&s.grabDocument&&d!=*s.grabDocument)s.clearGrab("Sound changed; grab again");
    if(s.sketching&&s.sketchContext&&d!=s.sketchContext->document)s.clearSketch("Sound changed; draw again");
    if(s.retired)return;s.slot=slot;s.sound=d;if(s.readRate()&&s.sketching)s.clearSketch("Sample rate changed; draw again");
    RoutedPatch patch;if(!makeRoutedPatch(d,patch)){s.invalidateComparison();suspend();if(s.retired)return;s.slot.reset();setEnabled(false);return;}s.settings=patch.effects[slot].dynamicEq;s.slotBypassed=patch.effects[slot].bypassed;
    auto base=std::make_unique<SoundDocument>(d);base->macros={};if(makeRoutedPatch(*base,patch))s.base=patch.effects[slot].dynamicEq;
    setEnabled(true);if(s.listening&&(!s.base.bands[s.selected].active||s.slotBypassed||(s.detectorMode&&!s.detectorAvailable())))s.audition(false);
    s.updateProposal();s.sync();repaint();
}
void DynamicEqGraph::showProposal(std::shared_ptr<const SoundDocument> p){if(state->retired)return;if(p&&state->grabbing)state->clearGrab("Review the proposal before grabbing");if(p&&state->sketching)state->clearSketch("Review the AI proposal before sketching");state->candidate=std::move(p);state->updateProposal();state->sync();repaint();}
void DynamicEqGraph::selectBand(std::size_t n){
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(n>=8||s.retired||s.closing()||!s.slot)return;s.clearGrab();s.end();if(s.retired||s.closing()||!s.slot)return;if(s.listening)s.audition(false);s.selected=n;s.selection.reset();s.selection.set(n);s.sync();repaint();
    const auto notify=onBandSelected;if(notify)notify(n);
}
std::size_t DynamicEqGraph::selectedBand() const{return state->selected;}
void DynamicEqGraph::setSelectedBands(std::bitset<8> bands){
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing()||!s.slot)return;s.clearGrab();s.end();if(s.retired||s.closing()||!s.slot)return;
    s.editStatus.clear();s.selection=bands;if(bands.any()&&!bands[s.selected])for(std::size_t n=0;n<8;++n)if(bands[n]){s.selected=n;break;}s.sync();repaint();
}
std::bitset<8> DynamicEqGraph::selectedBands() const noexcept{return state->selection;}
bool DynamicEqGraph::copySelectedBands(juce::String& out) const{const auto keep=state;const auto context=keep->capture();return context&&keep->copy(*context,keep->selection,out);}
bool DynamicEqGraph::pasteBands(const juce::String& text){const auto keep=state;const auto scope=keep->frame();const bool ok=keep->paste(keep->capture(),text);if(!ok)keep->report();return ok;}
std::optional<std::uint8_t> DynamicEqGraph::boundSlot() const{return state->slot;}
bool DynamicEqGraph::storeComparison(bool bankB){const auto keep=state;return keep->storeComparison(bankB);}
bool DynamicEqGraph::recallComparison(bool bankB){const auto keep=state;return keep->recallComparison(bankB);}
void DynamicEqGraph::clearComparison(){const auto keep=state;const auto scope=keep->frame();keep->invalidateComparison();if(!keep->retired&&!keep->closing()){keep->sync();repaint();}}
juce::Rectangle<float> DynamicEqGraph::plotBounds() const{return {52,56,std::max(1.f,float(getWidth())-108),std::max(1.f,float(getHeight())-130)};}
juce::Point<float> DynamicEqGraph::nodePosition(std::size_t n) const{if(n>=8)return {};const auto& b=state->base.bands[n];const auto r=plotBounds();return {xFor(r,b.frequencyHz),yFor(r,hasGain(b.shape)?b.gainDb:0)};}
std::optional<std::complex<double>> DynamicEqGraph::responseAt(double hz,bool proposed) const{return componentResponseAt(hz,DynamicEqPlacement::stereo,proposed);}
std::optional<std::complex<double>> DynamicEqGraph::componentResponseAt(double hz,DynamicEqPlacement component,bool proposed) const{const auto& s=*state;if(!s.slot||(proposed&&!s.proposal))return {};return componentTransfer(proposed?*s.proposal:s.settings,s.rate,hz,component);}
bool DynamicEqGraph::proposed() const{return state->marked;}
std::vector<std::size_t> DynamicEqGraph::parameters() const{std::vector<std::size_t> out;if(state->slot){for(std::size_t b=0;b<8;++b)for(std::size_t f:{0,1,3,4,5})out.push_back(state->field(b,f));out.push_back(state->gainField(0));out.push_back(state->gainField(1));}return out;}
void DynamicEqGraph::setSpectrumOn(bool on){auto& s=*state;if(s.retired)return;s.display=on;if(!on){s.clearGrab();if(s.token)(void)s.hub->setSpectrumEnabled(*s.token,false);if(!s.listening&&!s.detectorMode)s.unsubscribe();}else updateMonitoring();s.sync();repaint();}
bool DynamicEqGraph::spectrumOn() const{return state->display;}
void DynamicEqGraph::setDetectorAuditionMode(bool enabled){
    const auto keep=state;auto& s=*keep;if(s.retired||s.detectorMode==enabled)return;const auto scope=s.frame();
    if(s.listening)s.audition(false);s.detectorMode=enabled;if(!enabled&&!s.display)s.unsubscribe();
    if(!s.retired&&!s.closing()){s.sync();repaint();}
}
bool DynamicEqGraph::detectorAuditionMode() const noexcept {return state->detectorMode;}
void DynamicEqGraph::setGrabEnabled(bool on){
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing()||!s.slot)return;
    if(on&&s.sketching)s.clearSketch();
    s.clearGrab();s.end();if(s.retired||s.closing()||!s.slot)return;
    if(on&&s.display&&s.visible()&&!s.slotBypassed&&!s.proposal){
        s.audition(false);if(s.retired||s.closing())return;
        juce::String reason;if(s.grabDestination(s.sound,reason)){s.grabbing=true;s.grabStatus="Waiting for a complete spectrum";s.freezeGrab();grabKeyboardFocus();}
        else s.grabStatus=reason;
    }
    if(!s.retired&&!s.closing()){s.sync();repaint();}
}
bool DynamicEqGraph::grabEnabled() const noexcept{return state->grabbing;}
void DynamicEqGraph::setSketchEnabled(bool enabled){
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing())return;
    if(!enabled){cancelSketch();return;}if(s.matching)s.clearSketch();if(s.sketching)return;
    (void)s.readRate();
    const auto context=s.capture();
    if(!context||s.candidate){s.editStatus="Sketch: finish the current review or edit first";s.sync();repaint();return;}
    const auto source=s.rawEq(*context);
    if(!source||source->gainScale==0||s.sketchDestinations(*context).none()){
        s.editStatus=source&&source->gainScale==0?"Sketch: raise Gain Scale above zero first":"Sketch: no unlocked, unassigned unused bands";s.sync();repaint();return;
    }
    s.clearGrab();s.audition(false);s.clearSelection();if(s.retired||s.closing())return;
    s.sketching=true;s.sketchContext=s.capture(true);s.sketchStatus=hasComponentBands(s.settings)?"Draw stereo energy; new bands use Stereo":"Draw a static EQ curve; existing bands stay unchanged";s.editStatus.clear();s.sync();repaint();grabKeyboardFocus();
}
bool DynamicEqGraph::sketchEnabled() const noexcept{return state->sketching&&!state->matching;}
bool DynamicEqGraph::previewSketch(const EqCurve& target){
    const auto keep=state;const auto scope=keep->frame();if(!keep->sketching||keep->matching)return false;
    keep->sketchDrawing=false;keep->sketchPointsSize=0;return keep->queueSketch(target,keep->capture(true));
}
bool DynamicEqGraph::sketchFitting() const noexcept{return state->sketchJob&&!state->sketchJob->done.load(std::memory_order_acquire);}
bool DynamicEqGraph::sketchPreviewReady() const noexcept{return state->sketching&&state->sketchFit&&state->sketchContext&&state->current(*state->sketchContext);}
std::optional<float> DynamicEqGraph::sketchFitError() const noexcept{return state->sketching?state->sketchError:std::optional<float>{};}
std::optional<EqCurve> DynamicEqGraph::sketchTarget() const{return state->sketchTarget;}
std::size_t DynamicEqGraph::sketchPointCount() const noexcept{return state->sketchPointsSize;}
bool DynamicEqGraph::applySketch(){return !state->matching&&state->applySketch();}
bool DynamicEqGraph::previewMatch(const EqReference& source,const EqReference& reference,float amount){
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();
    if(s.retired||s.closing()||s.candidate||(s.sketching&&!s.matching))return false;
    if(s.matching)s.clearSketch();(void)s.readRate();
    const auto correction=eqMatchCorrection(source,reference,amount);if(!correction)return false;
    s.clearGrab();s.audition(false);s.clearSelection();if(s.retired||s.closing())return false;
    const auto context=s.capture();if(!context)return false;
    RoutedPatch patch;if(!makeRoutedPatch(context->document,patch))return false;const auto& effective=patch.effects[context->slot].dynamicEq;
    if(s.sketchDestinations(*context).none()||effective.gainScale==0)return false;
    PreparedDynamicEqResponse response;if(!response.prepare(effective,s.rate))return false;
    EqCurve base,target;
    for(std::size_t i=0;i<129;++i){const auto frequency=20*std::pow(1000.,double(i)/128);if(frequency>s.rate*.5)continue;
        const auto energy=response.energy(frequency);if(!energy)return false;base.db[i]=float(20*std::log10(std::max(*energy,1e-12)));target.db[i]=base.db[i]+correction->db[i];}
    s.sketching=s.matching=true;s.matchBase=base;
    if(!s.queueSketch(target,context)){s.clearSketch();return false;}return true;
}
bool DynamicEqGraph::applyMatch(){return state->matching&&state->applySketch();}
void DynamicEqGraph::cancelMatch(){if(state->matching)cancelSketch();}
DynamicEqGraph::MatchPreview DynamicEqGraph::matchPreview() const{
    const auto& s=*state;MatchPreview result;if(s.retired||s.closing()||!s.slot)return result;
    State::EditContext context;context.document=s.sound;context.slot=*s.slot;result.available=s.sketchDestinations(context).count();
    if(!s.matching||!s.sketchContext||!s.current(*s.sketchContext))return result;
    result.active=true;result.fitting=bool(s.sketchJob);result.canApply=bool(s.sketchFit)&&!s.candidate;result.added=s.sketchAdded.count();result.rmsDb=s.sketchError;result.fit=s.matchFit;result.status=s.sketchStatus;
    if(s.sketchTarget&&s.matchBase){result.target=*s.sketchTarget;for(std::size_t i=0;i<129;++i)result.target->db[i]-=s.matchBase->db[i];}return result;
}
void DynamicEqGraph::cancelSketch(){const auto keep=state;const auto scope=keep->frame();keep->clearSketch();if(!keep->retired&&!keep->closing()){keep->sync();repaint();}}
std::size_t DynamicEqGraph::grabPeakCount() const noexcept{return state->grabPeaks.size;}
juce::Point<float> DynamicEqGraph::grabPeakPosition(std::size_t n) const{
    const auto& s=*state;if(n>=s.grabPeaks.size)return {};const auto& peak=s.grabPeaks.values[n];const auto r=plotBounds();
    return {xFor(r,peak.frequencyHz),r.getY()+r.getHeight()*(-std::clamp(peak.levelDb,-90.f,0.f)/90.f)};
}
std::optional<FxAnalysisHub::Lease> DynamicEqGraph::subscription() const{return state->token&&state->hub->result(*state->token)?state->token:std::nullopt;}
std::shared_ptr<FxAnalysisHub> DynamicEqGraph::analysisHub() const{return state->hub;}
const FxSpectrum::Result& DynamicEqGraph::spectrum() const{return state->analysisResult();}
void DynamicEqGraph::updateMonitoring(){
    auto& s=*state;if(s.retired)return;const bool rateChanged=s.readRate();if(rateChanged){if(s.grabbing)s.clearGrab("Sample rate changed; grab again");if(s.sketching){s.clearSketch("Sample rate changed; draw again");s.sync();}repaint();}
    if(s.matching){s.pollSketch();s.unsubscribe();return;}
    const bool visible=s.visible(),metering=s.detectorMode&&isShowing(),drawSpectrum=s.display&&visible;
    if(s.closing()||!s.slot||(!visible&&!metering)){suspend();return;}
    if(!visible&&s.listening)s.audition(false);
    if(s.sketching)s.pollSketch();
    if(!drawSpectrum&&!s.listening&&!metering){s.unsubscribe();return;}
    if(!s.token)s.token=s.hub->acquire(*s.slot);
    if(s.token){
        if(s.listening&&!s.hub->requestedAudition(*s.token)){s.listening=false;s.sync();if(!drawSpectrum&&!metering){s.unsubscribe();return;}}
        (void)s.hub->setSpectrumEnabled(*s.token,drawSpectrum);if(s.privateHub)s.hub->update();
    }
    if(s.detectorMode)s.sync();
    if(s.grabbing){
        if(s.frozen&&(!s.analysisResult().valid||!s.grabContextCurrent()))s.clearGrab("Sound or spectrum changed; grab again");
        else s.freezeGrab();s.sync();repaint();
    }
    const auto& result=s.analysisResult();
    if(s.shownGeneration!=result.generation||s.shownWindows!=result.windows||s.shownMovement!=result.movementDb){s.shownGeneration=result.generation;s.shownWindows=result.windows;s.shownMovement=result.movementDb;repaint();}
}
void DynamicEqGraph::suspend(){const auto keep=state;auto& s=*keep;const bool changed=s.sketching||s.grabbing||s.grabStatus.isNotEmpty()||s.selection.any()||s.marquee;s.clearGrab();s.clearSketch();s.unsubscribe();s.clearSelection();if(!s.retired&&!s.closing()&&changed)s.sync();}
void DynamicEqGraph::retire(){if(!state||state->retired)return;stopTimer();state->retired=true;
    state->menuOpen=false;++state->menuGeneration;
    state->invalidateComparison();
    for(auto* button:std::array<juce::Button*,9>{&state->listen,&state->grab,&state->more,&state->compareA,&state->compareB,&state->compareCopy,&state->autoGain,&state->sketchApply,&state->sketchCancel}){button->onClick=nullptr;button->onStateChange=nullptr;}
    state->gainScale.onDragStart=nullptr;state->gainScale.onValueChange=nullptr;state->gainScale.onDragEnd=nullptr;
    suspend();state->slot.reset();onBandSelected=nullptr;onEdited=nullptr;onOverview=nullptr;onMatch=nullptr;onSpectral=nullptr;}
void DynamicEqGraph::timerCallback(){updateMonitoring();}
void DynamicEqGraph::resized(){
    if(!state)return;auto& s=*state;
    s.compareA.setBounds(20,12,30,28);s.compareB.setBounds(52,12,30,28);s.compareCopy.setBounds(92,12,94,28);
    s.autoGain.setBounds(214,12,94,28);s.compensationLabel.setBounds(310,12,84,28);
    s.scaleLabel.setBounds(424,12,42,28);s.gainScale.setBounds(470,12,200,28);
    int x=getWidth()-218;s.grab.setBounds(x,12,54,28);s.listen.setBounds(x+64,12,66,28);s.more.setBounds(x+144,12,54,28);
    s.sketchApply.setBounds(getWidth()-184,12,72,28);s.sketchCancel.setBounds(getWidth()-100,12,72,28);
}
void DynamicEqGraph::paint(juce::Graphics& g){
    const auto& s=*state;const auto r=plotBounds();const auto frame=getLocalBounds().reduced(8,8);
    g.setColour(juce::Colour(0xff13191b));g.fillRoundedRectangle(frame.toFloat().reduced(3),3);drawDisplayRim(g,s.displayRim,frame);
    if(s.sketching){g.setColour(paper);g.setFont(juce::FontOptions(17));g.drawText("Sketch EQ",24,12,125,28,juce::Justification::centredLeft);g.setFont(juce::FontOptions(14));g.setColour(paper.withAlpha(.65f));g.drawText(hasComponentBands(s.settings)?"Draw stereo energy; apply to unused Stereo bands":"Draw, preview, then apply to unused bands",165,12,std::max(1,getWidth()-370),28,juce::Justification::centredLeft);}
    g.setColour(grid);g.drawHorizontalLine(44,18.f,float(getWidth()-18));g.setFont(juce::FontOptions(15));
    for(double hz:{20.,50.,100.,200.,500.,1000.,2000.,5000.,10000.,20000.}){const auto x=xFor(r,hz);g.setColour(grid);g.drawVerticalLine(int(x),r.getY(),r.getBottom());g.setColour(paper);g.drawText(hz<1000?juce::String(int(hz)):juce::String(hz/1000,0)+"k",int(x)-22,int(r.getBottom())+5,44,17,juce::Justification::centred);}
    for(int db:{-24,-12,0,12,24}){const auto y=yFor(r,db);g.setColour(db==0?grid.brighter(.3f):grid);g.drawHorizontalLine(int(y),r.getX(),r.getRight());g.setColour(paper);g.drawText(juce::String(db>0?"+":"")+juce::String(db),20,int(y)-8,26,16,juce::Justification::centredRight);}
    if(!s.slot)return;
    {
    juce::Graphics::ScopedSaveState clipped(g);g.reduceClipRegion(r.toNearestInt());
    const auto& displayed=s.frozen?*s.frozen:s.analysisResult();
    const auto displayedDb=[&](double hz,bool output){return FxSpectrum::dbAt(displayed,hz,output);};
    if(displayed.valid&&s.display)for(bool output:{false,true}){juce::Path path;for(int i=0;i<=420;++i){const auto hz=20*std::exp(logSpan*i/420),db=std::clamp(double(displayedDb(hz,output)),-90.,0.);const auto x=r.getX()+r.getWidth()*i/420.f,y=r.getY()+r.getHeight()*float(-db/90);if(i==0)path.startNewSubPath(x,y);else path.lineTo(x,y);}g.setColour(output?curve.withAlpha(.36f):inputInk.withAlpha(.55f));g.strokePath(path,juce::PathStrokeType(1));}
    auto draw=[&](const DynamicEqSettings& settings,const std::array<float,8>& movement,juce::Colour colour,float width,bool dashed,bool selected,std::size_t role){
        auto& cached=s.curves[role];
        const bool mixed=hasComponentBands(settings);const auto component=settings.bands[s.selected].placement;
        const bool reuse=cached.valid&&cached.settings==settings&&cached.movement==movement&&cached.rate==s.rate&&cached.selected==s.selected;
        if(!reuse)cached.valid=false;
        if(!reuse&&role==3)(void)s.actualResponse.prepare(settings,s.rate,movement);
        juce::Path path;for(int i=0;i<=420;++i){
            auto& db=cached.db[std::size_t(i)];
            if(!reuse){const auto hz=20*std::exp(logSpan*i/420);std::optional<std::complex<double>> h;
                if(selected)h=component==DynamicEqPlacement::stereo?dynamicEqBandResponse(settings.bands[s.selected],s.rate,hz,movement[s.selected],settings.gainScale):componentTransfer(settings,s.rate,hz,component,movement);
                else if(role==3){const auto energy=s.actualResponse.energy(hz);if(energy)h=std::complex<double>(*energy,0);}
                else if(mixed){const auto energy=dynamicEqEnergyResponse(settings,s.rate,hz,movement);if(energy)h=std::complex<double>(*energy,0);}
                else h=dynamicEqResponse(settings,s.rate,hz,movement);
                db=h?decibels(*h):std::numeric_limits<double>::quiet_NaN();}
            if(!std::isfinite(db))continue;
            const auto x=r.getX()+r.getWidth()*i/420.f,y=yFor(r,db);if(i==0)path.startNewSubPath(x,y);else path.lineTo(x,y);
        }
        if(!reuse){cached.settings=settings;cached.movement=movement;cached.rate=s.rate;cached.selected=s.selected;cached.valid=true;}
        g.setColour(colour);if(dashed){const float lengths[]{5,4};juce::Path broken;juce::PathStrokeType(width).createDashedStroke(broken,path,lengths,2);g.fillPath(broken);}else g.strokePath(path,juce::PathStrokeType(width));
    };
    std::array<float,8> endpoints{};endpoints[s.selected]=s.settings.bands[s.selected].rangeDb;
    if(s.sketching)draw(s.base,{},paper.withAlpha(.4f),1.2f,false,false,2);
    else {
        draw(s.settings,endpoints,rangeInk.withAlpha(.65f),1,true,true,0);draw(s.settings,{},curve.withAlpha(.35f),1,false,true,1);draw(s.settings,{},paper,1.6f,false,false,2);
        if(s.analysisResult().sampleRate>0&&s.processor.effectiveEqMode(*s.slot)!=DynamicEqProcessingMode::spectral)draw(s.settings,s.analysisResult().movementDb,curve,2,false,false,3);
    }
    if(s.sketchTarget){
        juce::Path target;bool started=false;
        for(std::size_t i=0;i<s.sketchTarget->db.size();++i){
            if(20.*std::pow(1000.,double(i)/128.)>s.rate*.5)continue;
            const float x=r.getX()+r.getWidth()*float(i)/128,y=yFor(r,s.sketchTarget->db[i]);
            if(!started){target.startNewSubPath(x,y);started=true;}else target.lineTo(x,y);
        }
        const float lengths[]{5,4};juce::Path dashed;juce::PathStrokeType(1.6f).createDashedStroke(dashed,target,lengths,2);g.setColour(rangeInk);g.fillPath(dashed);
    }
    if(s.sketchFit)draw(*s.sketchFit,{},curve,2.f,false,false,4);
    if(s.sketchDrawing&&s.sketchPointsSize){juce::Path stroke;for(std::size_t i=0;i<s.sketchPointsSize;++i){const auto p=s.sketchPoints[i];const auto x=r.getX()+r.getWidth()*p.x,y=yFor(r,p.y);if(i==0)stroke.startNewSubPath(x,y);else stroke.lineTo(x,y);}g.setColour(rangeInk);g.strokePath(stroke,juce::PathStrokeType(1.6f));}
    if(s.proposal)draw(*s.proposal,{},juce::Colours::white,1.5f,true,false,4);
    if(s.grabPreview&&s.grabDocument&&s.grabSelected<s.grabPeaks.size){juce::String reason;if(const auto destination=s.grabDestination(*s.grabDocument,reason)){
        auto preview=s.settings;auto& band=preview.bands[*destination];band={};band.active=true;band.frequencyHz=float(s.grabPeaks.values[s.grabSelected].frequencyHz);band.q=s.grabPeaks.values[s.grabSelected].q;band.gainDb=s.grabGain;
        draw(preview,{},juce::Colours::white,1.5f,true,false,4);
    }}
    for(std::size_t n=0;n<8;++n){const auto& b=s.base.bands[n];if(!b.active)continue;const auto at=nodePosition(n);const auto colour=b.bypassed?paper.withAlpha(.35f):s.selection[n]?curve:paper;
        const auto movement=s.analysisResult().movementDb[n];if(hasGain(b.shape)&&movement!=0){g.setColour(rangeInk);g.drawLine(at.x,at.y,at.x,yFor(r,b.gainDb+movement),2);}
        if(s.selection[n]){g.setColour(curve.withAlpha(.16f));g.fillEllipse(at.x-11,at.y-11,22,22);}
        g.setColour(juce::Colour(0xff13191b));g.fillEllipse(at.x-7,at.y-7,14,14);g.setColour(colour);g.drawEllipse(at.x-7,at.y-7,14,14,s.selection[n]?2.f:1.f);g.setFont(juce::FontOptions(11));g.drawText(juce::String(int(n+1)),int(at.x)-6,int(at.y)-7,12,14,juce::Justification::centred);
        if(b.placement!=DynamicEqPlacement::stereo&&r.getWidth()>30&&r.getHeight()>20){
            const char* badges[]{"","L","R","M","S"};const float x=at.x+21<r.getRight()?at.x+10:at.x-21;
            const auto y=std::clamp(at.y-6,r.getY()+1,r.getBottom()-13);g.setFont(juce::FontOptions(10));
            g.drawText(badges[unsigned(b.placement)],int(x),int(y),11,12,juce::Justification::centred);
        }
    }
    if(s.marquee){const auto area=juce::Rectangle<float>(s.marqueeStart,s.marqueeEnd).getIntersection(r);g.setColour(curve.withAlpha(.1f));g.fillRect(area);g.setColour(curve.withAlpha(.7f));g.drawRect(area,1.f);}
    if(s.proposal)for(std::size_t n=0;n<8;++n){const auto& band=s.proposal->bands[n];if(band.active&&band!=s.settings.bands[n]){juce::Path ghost;ghost.addEllipse(xFor(r,band.frequencyHz)-10,yFor(r,hasGain(band.shape)?band.gainDb:0)-10,20,20);ProgrammerLook::drawDashes(g,ghost,juce::Colours::white,3,3);}}
    if(s.sketchFit)for(std::size_t n=0;n<8;++n)if(s.sketchAdded[n]){
        const auto& band=s.sketchFit->bands[n];const auto x=xFor(r,band.frequencyHz),y=yFor(r,band.gainDb);
        g.setColour(curve.withAlpha(.15f));g.fillEllipse(x-9,y-9,18,18);g.setColour(curve);g.drawEllipse(x-9,y-9,18,18,1.3f);
        g.setFont(juce::FontOptions(11));g.drawText(juce::String(int(n+1)),int(x)-6,int(y)-7,12,14,juce::Justification::centred);
    }
    for(std::size_t n=0;n<s.grabPeaks.size;++n){const auto at=grabPeakPosition(n);g.setColour(juce::Colour(0xff13191b));g.fillEllipse(at.x-5,at.y-5,10,10);g.setColour(n==s.grabSelected?rangeInk:inputInk);g.drawEllipse(at.x-5,at.y-5,10,10,n==s.grabSelected?2.f:1.f);g.setFont(juce::FontOptions(11));g.drawText("P"+juce::String(int(n+1)),int(at.x)-14,int(at.y)-21,28,14,juce::Justification::centred);}
    if(s.grabPreview&&s.grabSelected<s.grabPeaks.size){const auto at=grabPeakPosition(s.grabSelected);juce::Path ghost;ghost.addEllipse(at.x-9,yFor(r,s.grabGain)-9,18,18);ProgrammerLook::drawDashes(g,ghost,juce::Colours::white,3,3);}
    const auto cap=std::min(20000.,.45*s.rate);if(cap<20000){const auto x=xFor(r,cap);g.setColour(juce::Colours::white.withAlpha(.07f));g.fillRect(juce::Rectangle<float>(x,r.getY(),r.getRight()-x,r.getHeight()));}
    }
    const auto legendY=getHeight()-50,legendRight=getWidth()-28;
    const bool mixed=hasComponentBands(s.settings);
    g.setFont(juce::FontOptions(13));g.setColour(inputInk);g.drawText(s.frozen?"Frozen input":mixed?"Stereo input":"Slot input",legendRight-(mixed?238:210),legendY,95,14,juce::Justification::centredRight);
    g.setColour(curve);g.drawText(s.frozen?"Frozen output":mixed?"Stereo output dBFS":"Slot output dBFS",legendRight-(mixed?128:110),legendY,mixed?128:110,14,juce::Justification::centredRight);
    g.setColour(paper);g.setFont(juce::FontOptions(12));g.drawText("dB",12,int(r.getY())-9,28,12,juce::Justification::centredRight);
    for(int db:{0,-30,-60,-90})g.drawText(juce::String(db),int(r.getRight())+8,int(r.getY()+r.getHeight()*float(-db)/90)-8,28,16,juce::Justification::centredLeft);
    const bool spectral=s.slot&&s.processor.effectiveEqMode(*s.slot)==DynamicEqProcessingMode::spectral;
    const char* legends[]{spectral?"Guide":mixed?"Energy":"Base",spectral?"Static":"Live",s.settings.bands[s.selected].placement==DynamicEqPlacement::stereo?"Band":placementName(s.settings.bands[s.selected].placement),"Range"};const juce::Colour colours[]{paper,curve,curve.withAlpha(.4f),rangeInk};
    if(s.sketching){const char* sketchLegends[]{"Current","Target","Fit"};const juce::Colour sketchColours[]{paper.withAlpha(.5f),rangeInk,curve};for(int n=0;n<3;++n){const auto x=52+n*95;g.setColour(sketchColours[n]);if(n==1){g.drawLine(float(x),float(legendY+8),float(x+5),float(legendY+8),1.5f);g.drawLine(float(x+9),float(legendY+8),float(x+14),float(legendY+8),1.5f);}else g.drawLine(float(x),float(legendY+8),float(x+14),float(legendY+8),1.5f);g.drawText(sketchLegends[n],x+19,legendY,70,16,juce::Justification::centredLeft);}}
    else for(int n=0;n<4;++n){const auto x=52+n*75;g.setColour(colours[n]);g.drawLine(float(x),float(legendY+8),float(x+12),float(legendY+8),1.5f);g.drawText(legends[n],x+16,legendY,56,16,juce::Justification::centredLeft);}
    if(s.proposal){g.setColour(juce::Colours::white);g.drawText("Dashed: proposal",365,legendY,150,16,juce::Justification::centredLeft);}
    const auto& b=s.base.bands[s.selected];const char* shapes[]{"Bell","Low shelf","High shelf","Low cut","High cut","Notch","Band pass","Tilt shelf","Flat tilt"};
    const auto hz=b.shape==DynamicEqShape::flatTilt?double(b.frequencyHz):std::min(double(b.frequencyHz),.45*s.rate);
    const auto q=b.shape==DynamicEqShape::lowShelf||b.shape==DynamicEqShape::highShelf||b.shape==DynamicEqShape::tiltShelf?std::min(b.q,4.f):b.q;
    auto readout=juce::String(s.slotBypassed?"SLOT BYPASSED   Band ":s.listening?(s.detectorMode?"TRIGGER   Band ":"AUDITION   Band "):"Band ")+juce::String(int(s.selected+1))+"   "+shapes[int(b.shape)]+"   "+juce::String(hz,0)+" Hz";
    if(b.placement!=DynamicEqPlacement::stereo)readout=juce::String(placementName(b.placement))+"   "+readout;
    if(hasGain(b.shape)){
        readout+="   "+juce::String(b.gainDb,1)+" dB";
        if(hasQ(b.shape))readout+="   Q "+juce::String(q,2);
        readout+=spectral&&b.spectralEnabled?"   max bin movement "+juce::String(s.analysisResult().spectralMovementDb[s.selected],1)+" dB":hasDynamics(b.shape)?"   movement "+juce::String(s.analysisResult().movementDb[s.selected],1)+" dB":"   static";
    }else if(hasQ(b.shape))readout+="   Q "+juce::String(q,2)+"   static";
    else {const int slopes[]{12,24,6,36,48,96};readout+="   "+juce::String(slopes[int(b.slope)])+" dB/oct   static";}
    if(q!=b.q||hz!=b.frequencyHz)readout+=" (effective limits)";
    if(s.selection.count()>1)readout=juce::String(int(s.selection.count()))+" bands selected   Drag or arrows: frequency / gain   Wheel: Q   Ctrl+C / Ctrl+V";
    if(s.sketching)readout=s.sketchStatus+"   Esc cancels";
    else if(s.grabbing)readout=s.grabPreview?"NEW BELL   "+juce::String(s.grabPeaks.values[s.grabSelected].frequencyHz,0)+" Hz   "+juce::String(s.grabGain,1)+" dB   Release or Enter creates; Escape cancels":"GRAB   "+s.grabStatus;
    else if(s.grabStatus.isNotEmpty())readout="GRAB   "+s.grabStatus;
    else if(s.editStatus.isNotEmpty())readout=s.editStatus;
    g.setFont(juce::FontOptions(13));g.setColour(s.editStatus.isNotEmpty()?rangeInk:paper);g.drawText(readout,52,getHeight()-35,getWidth()-335,16,juce::Justification::centredLeft);
    const bool macro=s.settings!=s.base;
    const auto hostRate=s.rate/(s.processor.effectiveRenderQuality()==RenderQuality::higher?2:1);
    const auto note=spectral?juce::String(s.processor.getLatencySamples()*1000./hostRate,1)+" ms prepared":!s.rateKnown?juce::String("48 kHz preview"):juce::String(s.rate/1000,1)+" kHz internal"+(macro?" | macros":"");
    g.setFont(juce::FontOptions(12));g.setColour(paper.withAlpha(.6f));g.drawText(note,getWidth()-270,getHeight()-35,242,16,juce::Justification::centredRight);
}
void DynamicEqGraph::mouseDown(const juce::MouseEvent& e){const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing()||!s.slot||!plotBounds().expanded(9).contains(e.position))return;
    if(s.sketching){if(e.mods.isPopupMenu()){cancelSketch();return;}if(e.mods.isLeftButtonDown()&&plotBounds().contains(e.position)){s.beginSketchStroke(e.position);grabKeyboardFocus();if(!s.retired&&!s.closing())repaint();}return;}
    if(e.mods.isPopupMenu()){if(!s.grabbing)s.menu();return;}if(!e.mods.isLeftButtonDown())return;
    if(s.grabbing){std::optional<std::size_t> nearest;float distance=13;for(std::size_t n=0;n<s.grabPeaks.size;++n){const auto d=grabPeakPosition(n).getDistanceFrom(e.position);if(d<distance){nearest=n;distance=d;}}
        if(nearest){s.grabSelected=*nearest;s.grabStartY=e.position.y;s.grabGain=0;s.grabDragging=s.grabPreview=true;grabKeyboardFocus();if(!s.retired&&!s.closing())repaint();}return;}
    std::optional<std::size_t> nearest;float distance=14;for(std::size_t n=0;n<8;++n)if(s.base.bands[n].active){const auto d=nodePosition(n).getDistanceFrom(e.position);if(d<distance){nearest=n;distance=d;}}
    if(nearest){
        if(e.mods.isShiftDown()){auto bands=s.selection;bands.flip(*nearest);setSelectedBands(bands);if(!s.retired&&!s.closing())grabKeyboardFocus();return;}
        if(!s.selection[*nearest]||s.selection.count()<2)selectBand(*nearest);
        else {s.end();if(s.retired||s.closing())return;s.selected=*nearest;s.sync();repaint();}
        if(s.retired||s.closing()||!s.slot)return;s.dragBand=*nearest;if(s.beginNode(e.position))grabKeyboardFocus();
    }else if(plotBounds().contains(e.position)){
        s.end();if(s.retired||s.closing())return;s.marqueeBefore=e.mods.isShiftDown()?s.selection:std::bitset<8>{};s.selection=s.marqueeBefore;
        s.marquee=true;s.marqueeStart=s.marqueeEnd=e.position;grabKeyboardFocus();s.sync();repaint();
    }
}
void DynamicEqGraph::mouseDrag(const juce::MouseEvent& e){const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing())return;if(s.grabDragging){s.grabGain=std::clamp((s.grabStartY-e.position.y)*60/std::max(1.f,plotBounds().getHeight()),-24.f,24.f);repaint();return;}
    if(s.sketching){if(s.sketchDrawing){s.addSketchPoint(e.position);repaint();}return;}
    if(s.marquee){s.marqueeEnd=e.position;s.selection=s.marqueeBefore;const auto area=juce::Rectangle<float>(s.marqueeStart,s.marqueeEnd).getIntersection(plotBounds());for(std::size_t n=0;n<8;++n)if(s.base.bands[n].active&&area.contains(nodePosition(n)))s.selection.set(n);s.sync();repaint();return;}s.nodeEdit(e.position);
}
void DynamicEqGraph::mouseUp(const juce::MouseEvent& e){const auto keep=state;if(keep->sketching&&keep->sketchDrawing){keep->addSketchPoint(e.position);keep->finishSketchStroke();if(!keep->retired&&!keep->closing())repaint();return;}if(keep->grabDragging)keep->commitGrab();else {keep->marquee=false;keep->end();if(!keep->retired&&!keep->closing())repaint();}}
void DynamicEqGraph::mouseDoubleClick(const juce::MouseEvent& e){const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(!s.retired&&!s.grabbing&&!s.sketching&&e.mods.isLeftButtonDown()&&plotBounds().contains(e.position)){for(std::size_t n=0;n<8;++n)if(s.base.bands[n].active&&nodePosition(n).getDistanceFrom(e.position)<14)return;s.addAt(e.position);}}
void DynamicEqGraph::mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& wheel){if(rackScrolling())return juce::Component::mouseWheelMove(e,wheel);const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(!s.slot||s.retired||!plotBounds().contains(e.position)||wheel.deltaY==0)return;
    if(s.grabbing||s.sketching)return;
    s.wheelQ(wheel.deltaY);}
bool DynamicEqGraph::keyPressed(const juce::KeyPress& key){const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||!s.slot)return false;
    if(key==juce::KeyPress::escapeKey){s.clearSketch();s.clearGrab();s.clearSelection();s.audition(false);s.sync();repaint();return true;}
    const auto code=key.getKeyCode();
    if((code=='S'||code=='s')&&!key.getModifiers().isCtrlDown()&&!key.getModifiers().isAltDown()&&!key.getModifiers().isCommandDown()){setSketchEnabled(!s.sketching);return true;}
    if(s.sketching){if(code==juce::KeyPress::returnKey){(void)applySketch();return true;}return code==juce::KeyPress::deleteKey||code==juce::KeyPress::backspaceKey||code==juce::KeyPress::leftKey||code==juce::KeyPress::rightKey||code==juce::KeyPress::upKey||code==juce::KeyPress::downKey;}
    if(s.grabbing){
        if(!s.grabPeaks.size)return true;
        if(code==juce::KeyPress::returnKey){s.commitGrab();return true;}
        if(code==juce::KeyPress::leftKey||code==juce::KeyPress::rightKey){s.grabSelected=(s.grabSelected+s.grabPeaks.size+(code==juce::KeyPress::leftKey?-1:1))%s.grabPeaks.size;s.grabGain=0;s.grabPreview=false;repaint();return true;}
        if(code==juce::KeyPress::upKey||code==juce::KeyPress::downKey){s.grabGain=std::clamp(s.grabGain+(code==juce::KeyPress::upKey?1.f:-1.f)*(key.getModifiers().isShiftDown()?.1f:.5f),-24.f,24.f);s.grabPreview=true;repaint();return true;}
        return code==juce::KeyPress::deleteKey||code==juce::KeyPress::backspaceKey;
    }
    if((key.getModifiers().isCtrlDown()||key.getModifiers().isCommandDown())&&(code=='C'||code=='c'||code=='V'||code=='v')){
        s.clipboardCommand(code=='V'||code=='v',s.capture(),s.selection);return true;
    }
    if(code==juce::KeyPress::deleteKey||code==juce::KeyPress::backspaceKey){s.setBandFields(0,0);return true;}
    if(code!=juce::KeyPress::leftKey&&code!=juce::KeyPress::rightKey&&code!=juce::KeyPress::upKey&&code!=juce::KeyPress::downKey)return false;
    const auto step=key.getModifiers().isShiftDown()?1.f:5.f;auto at=nodePosition(s.selected);if(code==juce::KeyPress::leftKey)at.x-=step;if(code==juce::KeyPress::rightKey)at.x+=step;if(code==juce::KeyPress::upKey)at.y-=step;if(code==juce::KeyPress::downKey)at.y+=step;
    if(s.beginNode(nodePosition(s.selected))){s.nodeEdit(at);s.end();}return true;
}
void DynamicEqGraph::focusLost(FocusChangeType){
    const auto keep=state;if(!keep||keep->retired)return;const auto scope=keep->frame();
    if(keep->sketchApplying)return;
    // A completed stroke is an explicit Apply/Cancel preview. Keyboard focus
    // alone does not change its document or invalidate that decision.
    if(keep->sketching&&!keep->sketchDrawing)return;
    const bool childFocus=hasKeyboardFocus(true)&&!hasKeyboardFocus(false);keep->clearGrab();
    if(!childFocus&&!keep->menuOpen&&keep->sketching)keep->clearSketch();
    // The owned popup temporarily takes focus; its captured selection remains
    // valid. Reset/rebind still invalidate the command through its context.
    if(childFocus||keep->menuOpen)keep->end();else keep->clearSelection();
    if(keep->retired||keep->closing())return;keep->audition(false);keep->sync();repaint();
}
void DynamicEqGraph::visibilityChanged(){if(state&&!state->visible())suspend();}
std::unique_ptr<juce::AccessibilityHandler> DynamicEqGraph::createAccessibilityHandler(){return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::group);}
}
