#include "SaturationGraph.h"
#include "MakerFace.h"
#include "InstrumentProcessor.h"
#include "FxMonitor.h"
#include "DotMatrix.h"
#include "ProgrammerLook.h"
#include <algorithm>
#include <cmath>

namespace airack::studio
{
namespace
{
juce::Uuid ownerOf(const SoundDocument& d,std::uint8_t slot)
{
    if(d.personalities)for(const auto& rack:d.personalities->instances)for(const auto& unit:rack.design.units)
        if(unit.module==NativeModule::saturation)if(const auto* binding=rackUnitBinding(rack,unit.id);binding&&binding->slot==slot)return rack.id;
    return juce::Uuid::null();
}
bool saturationSlot(const SoundDocument& d,std::uint8_t slot)
{
    return slot<8&&d.layout.effects[slot].use==SlotUse::present&&d.layout.effects[slot].kind==EffectKind::saturation;
}
juce::String hz(float value){return juce::String(value,0)+" Hz";}
}

struct SaturationGraph::State : std::enable_shared_from_this<State>, private juce::ComponentListener
{
    SaturationGraph& view;
    InstrumentProcessor& processor;
    house::Host host;
    MakerSkinPtr equipmentOwner;
    std::shared_ptr<FxMonitor> monitor;
    std::optional<std::uint8_t> slot;
    std::optional<FxMonitor::SaturationToken> auditionToken;
    std::optional<SaturationLane> auditionBand;
    SoundDocument sound,expected;
    ParameterValues effective{};
    std::shared_ptr<const SoundDocument> proposal;
    std::vector<juce::Component::SafePointer<juce::Component>> ancestors;
    std::uint64_t revision=0,reset=0,observedReset=0;
    std::size_t leader=parameterCount;
    bool resetKnown=false,retired=false,active=false,inCall=false,abort=false,rateKnown=false;
    int selected=0;
    double rate=48000;

    struct HoldButton final:juce::TextButton {
        std::weak_ptr<State> owner;
        const house::Model* equipment=nullptr;
        int keyboardHeld=0;
        explicit HoldButton(const char* name):juce::TextButton(name){}
        void paintButton(juce::Graphics& g,bool hover,bool down) override {
            const auto area=getLocalBounds().toFloat().reduced(1);
            if(equipment)house::paintNineSlice(g,equipment->asset("advanced:key"),area.toNearestInt());
            else {g.setColour(juce::Colour(0xff252c2d));g.fillRoundedRectangle(area,2);}
            const juce::Colour ink(0xffe5e6e1),held(0xffceaf7d);
            if(hover||down){g.setColour(held.withAlpha(down?.14f:.05f));g.fillRoundedRectangle(area,2);}
            g.setColour(down?held:ink);g.drawRoundedRectangle(area,2,down?2.f:1.f);
            const auto* model=studioSkin();g.setFont(model?Lettering(model->screenFace).fontForCap(14):juce::Font(juce::FontOptions(16)));
            g.drawText(getButtonText(),getLocalBounds().reduced(4,2),juce::Justification::centred);
            if(down)g.drawLine(12,float(getHeight()-4),float(getWidth()-12),float(getHeight()-4),2);
        }
        bool keyPressed(const juce::KeyPress& key) override {
            if(key.isKeyCode(juce::KeyPress::escapeKey)){if(const auto s=owner.lock())s->endBand();return true;}
            if(isEnabled()&&(key.isKeyCode(juce::KeyPress::spaceKey)||key.isKeyCode(juce::KeyPress::returnKey))){if(!keyboardHeld)keyboardHeld=key.getKeyCode();setState(buttonDown);return true;}
            return juce::TextButton::keyPressed(key);
        }
        bool keyStateChanged(bool down) override {
            if(keyboardHeld){
                const auto s=owner.lock();
                if(!s||!s->view.auditionKeyDown(keyboardHeld)){keyboardHeld=0;setState(buttonNormal);}
                return true;
            }
            return juce::TextButton::keyStateChanged(down);
        }
        // JUCE's mouse handlers do not know about our held activation key.
        void mouseEnter(const juce::MouseEvent& event) override{if(!keyboardHeld)juce::TextButton::mouseEnter(event);}
        void mouseExit(const juce::MouseEvent& event) override{if(!keyboardHeld)juce::TextButton::mouseExit(event);}
        void mouseDrag(const juce::MouseEvent& event) override{if(!keyboardHeld)juce::TextButton::mouseDrag(event);}
        void mouseUp(const juce::MouseEvent& event) override{if(!keyboardHeld)juce::TextButton::mouseUp(event);}
        void focusLost(FocusChangeType type) override {
            keyboardHeld=0;setState(buttonNormal);if(const auto s=owner.lock())s->endBand();juce::TextButton::focusLost(type);
        }
    };
    std::array<HoldButton,3> buttons{{HoldButton("LOW"),HoldButton("MID"),HoldButton("HIGH")}};

    State(SaturationGraph& v,InstrumentProcessor& p,house::Host h):view(v),processor(p),host(std::move(h)),monitor(p.fxMonitorHandle()){}
    ~State() override{unwatch();}
    std::shared_ptr<void> frame() const{return host.frame?host.frame():nullptr;}
    bool closing() const{return host.closing&&host.closing();}
    std::size_t index(int n) const{return saturationParameterStart+std::size_t(*slot)*saturationParametersPerSlot+5+std::size_t(n);}
    float raw(int n) const{return parameterPhysicalValue(sound.values,index(n));}
    bool locked() const{return !slot||sound.effectLocks[*slot]||sound.controlLocks[index(0)]||sound.controlLocks[index(1)];}
    bool visible() const{
        for(auto* c=static_cast<const juce::Component*>(&view);c;c=c->getParentComponent())if(!c->isVisible())return false;
        return true;
    }
    void unwatch(){for(const auto& c:ancestors)if(c)c->removeComponentListener(this);ancestors.clear();}
    void watch(){
        unwatch();if(retired)return;
        for(auto* c=view.getParentComponent();c;c=c->getParentComponent()){ancestors.emplace_back(c);c->addComponentListener(this);}
        if(!visible())cancel();
    }
    void componentVisibilityChanged(juce::Component&) override{const auto keep=shared_from_this();if(!visible())cancel();}
    void componentBeingDeleted(juce::Component&) override{const auto keep=shared_from_this();cancel();unwatch();}
    juce::Rectangle<float> plot() const{return {28,66,std::max(1.f,float(view.getWidth())-56),40};}
    float x(float frequency) const{return plot().getX()+plot().getWidth()*float(std::log(std::clamp(frequency,20.f,20000.f)/20.f)/std::log(1000.));}
    float frequency(float position) const{return 20.f*std::pow(1000.f,std::clamp((position-plot().getX())/plot().getWidth(),0.f,1.f));}
    std::array<float,2> preview(const ParameterValues& values) const{
        const auto high=std::min(parameterPhysicalValue(values,index(1)),float(.45*rate));
        return {std::min(parameterPhysicalValue(values,index(0)),.8f*high),high};
    }
    bool proposed(ParameterValues& values) const{
        return proposal&&slot&&proposal->layout==sound.layout&&ownerOf(*proposal,*slot)==ownerOf(sound,*slot)
            &&makeEffectiveParameterValues(*proposal,values);
    }
    bool marked(const ParameterValues& candidate) const{
        if(!slot||sound.effectLocks[*slot])return false;
        const auto liveCorners=preview(effective),candidateCorners=preview(candidate);
        for(int n=0;n<2;++n)if(!sound.controlLocks[index(n)]&&(proposal->values[index(n)]!=sound.values[index(n)]||candidateCorners[std::size_t(n)]!=liveCorners[std::size_t(n)]))return true;
        return false;
    }
    void describe(){
        if(retired||closing()||!slot)return;
        const auto corners=preview(effective);const auto base=index(0)-5;
        juce::String text="SATURATION | ";
        text+=parameterPhysicalValue(effective,base)==0?"Full mode: three-band settings retained/inactive. ":"Three-band mode: Full settings retained/inactive. ";
        text+="Raw Low "+hz(raw(0))+", High "+hz(raw(1))+". Preview (macros + rate clamps): Low "+hz(corners[0])+", High "+hz(corners[1])+". ";
        text+=rateKnown?hz(float(rate))+" effective processing rate. ":"Unknown effective processing rate; preview fallback "+hz(float(rate))+". ";
        for(const auto field:{8u,10u})if(parameterPhysicalValue(effective,base+field-1)==1){
            const auto requested=parameterPhysicalValue(effective,base+field);const auto cut=std::min(requested,float(.45*rate));
            text+=(field==8?"Pre low cut ":"Post high cut ")+hz(requested)+" -> preview "+hz(cut)+(cut<requested?" (clamped). ":". ");
        }
        if(locked())text+="Locked crossover gesture. ";
        ParameterValues candidate;if(proposed(candidate)&&marked(candidate))text+="AI proposal outlines raw and macro/rate preview changes; sound unchanged. ";
        text+="Up/Down selects Low/High; Left/Right edits the selected raw corner. ";
        text+="Hold Low/Mid/High to audition; overrides Global Mix. ";
        if(auditionBand)text+=buttons[std::size_t(*auditionBand)-1].getButtonText()+" held; release restores normal output. ";
        view.setTitle(selected==0?"Raw low crossover":"Raw high crossover");view.setDescription(text);
    }
    bool finish(){
        const auto keep=shared_from_this();
        if(inCall){abort=true;return false;}
        if(!active)return false;
        const auto scope=frame();const auto parameter=leader;active=false;abort=false;leader=parameterCount;
        inCall=true;const bool ok=processor.endParameterGesture(parameter);inCall=false;return ok;
    }
    void endBand(){
        const auto keep=shared_from_this();const auto scope=frame();const auto token=auditionToken;const auto previousBand=auditionBand;
        auditionToken.reset();auditionBand.reset();
        if(token)processor.retireSaturationAudition(*token);
        if(previousBand){auto& button=buttons[std::size_t(*previousBand)-1];button.keyboardHeld=0;if(button.isDown())button.setState(juce::Button::buttonNormal);}
        if(!retired&&!closing()){describe();view.repaint();}
    }
    void cancel(){endBand();finish();}
    bool canAudition() const {
        return !retired&&!closing()&&slot&&visible()&&rateKnown
            &&parameterPhysicalValue(effective,index(0)-5)==1&&parameterPhysicalValue(effective,572+*slot*23)==0;
    }
    bool beginBand(SaturationLane lane){
        const auto keep=shared_from_this();const auto scope=frame();
        if(inCall||!canAudition()||lane==SaturationLane::full||static_cast<unsigned>(lane)>static_cast<unsigned>(SaturationLane::high))return false;
        finish();if(retired||closing()||!slot)return false;
        endBand();if(retired||closing()||!slot||!canAudition())return false;
        if(!processor.trySoundSnapshot(expected,revision,reset)||expected!=sound||(resetKnown&&reset!=observedReset))return false;
        const auto rack=ownerOf(expected,*slot);const auto token=processor.acquireSaturationAudition(*slot,rack,revision,reset);
        if(!token)return false;
        if(!processor.requestSaturationAudition(*token,lane,rack,revision,reset)||retired||closing()||!visible()){
            processor.retireSaturationAudition(*token);return false;
        }
        auditionToken=token;auditionBand=lane;buttons[std::size_t(lane)-1].setState(juce::Button::buttonDown);describe();view.repaint();return true;
    }
    std::array<ParameterGestureValue,2> edits(float low,float high) const{
        return {{{index(0),low==raw(0)?sound.values[index(0)]:processor.control(index(0)).convertTo0to1(low)},
                 {index(1),high==raw(1)?sound.values[index(1)]:processor.control(index(1)).convertTo0to1(high)}}};
    }
    bool begin(){
        const auto keep=shared_from_this();const auto scope=frame();
        if(retired||closing()||!slot||!visible()||inCall||locked())return false;
        endBand();finish();if(retired||closing()||!slot)return false;
        if(!processor.trySoundSnapshot(expected,revision,reset)||expected!=sound||(resetKnown&&reset!=observedReset))return false;
        const auto values=edits(raw(0),raw(1));leader=index(0);abort=false;
        inCall=true;const bool ok=processor.beginParameterGesture(leader,values,expected,revision,reset);inCall=false;active=ok;
        if(!ok||retired||closing()||abort||!visible()){finish();return false;}return true;
    }
    bool write(float low,float high){
        const auto keep=shared_from_this();const auto scope=frame();
        if(!active||retired||closing()||!slot||!visible()||inCall){finish();return false;}
        if(!std::isfinite(low)||!std::isfinite(high)||low<40||low>2000||high<500||high>16000){finish();return false;}
        const auto values=edits(low,high);const auto oldReset=reset;
        inCall=true;const bool ok=processor.setParameterGestureValues(leader,values,expected,revision,reset);inCall=false;
        if(!ok||retired||closing()||abort){finish();return false;}
        SoundDocument next;std::uint64_t nextRevision,nextReset;
        if(!processor.trySoundSnapshot(next,nextRevision,nextReset)||nextReset!=oldReset||!saturationSlot(next,*slot)
            ||next.layout!=expected.layout||ownerOf(next,*slot)!=ownerOf(expected,*slot)){finish();return false;}
        // Host value listeners can change the context before this call returns.
        auto context=next;context.values[index(0)]=expected.values[index(0)];context.values[index(1)]=expected.values[index(1)];
        if(context!=expected||next.values[index(0)]!=values[0].value||next.values[index(1)]!=values[1].value){finish();return false;}
        expected=next;revision=nextRevision;reset=nextReset;view.refresh(next,*slot);return !retired&&!closing()&&active;
    }
    bool single(float value){
        const auto keep=shared_from_this();if(!slot||retired||closing())return false;
        const float low=selected==0?value:raw(0),high=selected==1?value:raw(1);
        if(!begin())return false;const bool ok=write(low,high);const bool ended=finish();return ok&&ended;
    }
    struct Accessible final:juce::AccessibilityValueInterface {
        std::weak_ptr<State> state;
        explicit Accessible(const std::shared_ptr<State>& s):state(s){}
        bool isReadOnly() const override{const auto s=state.lock();return !s||s->retired||s->closing()||s->locked()||!s->visible();}
        double getCurrentValue() const override{const auto s=state.lock();return s&&s->slot?s->raw(s->selected):0;}
        juce::String getCurrentValueAsString() const override{const auto s=state.lock();return s&&s->slot?hz(s->raw(s->selected)):juce::String();}
        void setValue(double v) override{if(const auto s=state.lock();s&&!isReadOnly()&&std::isfinite(v))s->single(float(v));}
        void setValueAsString(const juce::String& v) override{const auto text=v.trim();if(text.containsOnly("0123456789.+-"))setValue(text.getDoubleValue());}
        AccessibleValueRange getRange() const override{const auto s=state.lock();return {{s&&s->selected==1?500.:40.,s&&s->selected==1?16000.:2000.},1};}
    };
};

SaturationGraph::SaturationGraph(InstrumentProcessor& p,house::Host h,MakerSkinPtr equipment):state(std::make_shared<State>(*this,p,std::move(h)))
{
    state->equipmentOwner=std::move(equipment);
    setComponentID("saturation.graph");setWantsKeyboardFocus(true);setExplicitFocusOrder(200);setTitle("Raw low crossover");
    for(std::size_t n=0;n<3;++n){auto& button=state->buttons[n];button.owner=state;button.equipment=state->equipmentOwner?&state->equipmentOwner->skin:nullptr;button.setComponentID("saturation.audition."+juce::String(n+1));
        button.setTitle(button.getButtonText()+" band audition");button.setDescription("Hold to audition this band. Overrides Global Mix. Momentary monitoring only; release, Escape or focus loss restores normal output.");
        button.setClickingTogglesState(false);button.setColour(juce::TextButton::buttonColourId,juce::Colour(0xff252c2d));button.setColour(juce::TextButton::buttonOnColourId,juce::Colour(0xffceaf7d));
        const std::weak_ptr<State> weak=state;auto* source=&button;
        button.onStateChange=[weak,source,n]{if(const auto s=weak.lock();s&&!s->retired){
            if(source->isDown()){if(s->auditionBand!=SaturationLane(n+1)&&!s->beginBand(SaturationLane(n+1)))source->setState(juce::Button::buttonNormal);}
            else if(s->auditionBand==SaturationLane(n+1))s->endBand();
        }};addAndMakeVisible(button);
    }
}
SaturationGraph::~SaturationGraph(){retire();}
void SaturationGraph::setEquipmentArtwork(MakerSkinPtr owner)
{
    const auto previous=state->equipmentOwner;
    state->equipmentOwner=std::move(owner);
    for(auto& button:state->buttons){button.equipment=state->equipmentOwner?&state->equipmentOwner->skin:nullptr;button.repaint();}
}
void SaturationGraph::refresh(const SoundDocument& d,std::uint8_t slot)
{
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired)return;if(s.closing()){s.cancel();return;}
    if(!s.visible())s.cancel();if(s.retired||s.closing())return;
    SoundDocument live;std::uint64_t revision,reset;const bool snapshot=s.processor.trySoundSnapshot(live,revision,reset);
    if(s.auditionToken&&(!snapshot||revision!=s.revision||reset!=s.reset||live!=s.expected))s.endBand();
    if(snapshot){if(s.resetKnown&&s.observedReset!=reset)s.cancel();s.observedReset=reset;s.resetKnown=true;}
    if(s.retired||s.closing())return;
    ParameterValues projected;
    if(!saturationSlot(d,slot)||!makeEffectiveParameterValues(d,projected)){
        s.cancel();if(s.retired)return;s.slot.reset();setEnabled(false);setDescription("SATURATION | No bound saturation slot.");repaint();return;
    }
    if(s.slot&&(*s.slot!=slot||s.sound.layout!=d.layout||ownerOf(s.sound,*s.slot)!=ownerOf(d,slot)
        ||(s.active&&(d!=s.expected||(snapshot&&(revision!=s.revision||reset!=s.reset))))))s.cancel();
    if(s.retired||s.closing())return;
    const auto processingRate=s.monitor?s.monitor->sampleRate():0.;const bool known=std::isfinite(processingRate)&&processingRate>0;const auto rate=known?processingRate:48000.;
    if(s.auditionToken&&(s.rate!=rate||!known||d!=s.expected))s.endBand();
    const bool changed=!s.slot||*s.slot!=slot||s.sound!=d||s.effective!=projected||s.rate!=rate||s.rateKnown!=known;
    s.slot=slot;s.sound=d;s.effective=projected;s.rate=rate;s.rateKnown=known;setEnabled(true);
    if(!s.canAudition())s.endBand();for(auto& button:s.buttons)button.setEnabled(s.canAudition());
    if(s.auditionToken&&!s.processor.requestSaturationAudition(*s.auditionToken,s.auditionBand,ownerOf(s.expected,*s.slot),s.revision,s.reset))s.endBand();
    if(changed){s.describe();repaint();}
}
void SaturationGraph::showProposal(std::shared_ptr<const SoundDocument> p){const auto keep=state;if(keep->retired)return;keep->proposal=std::move(p);keep->describe();repaint();}
std::vector<std::size_t> SaturationGraph::parameters() const{const auto& s=*state;return !s.retired&&s.slot?std::vector<std::size_t>{s.index(0),s.index(1)}:std::vector<std::size_t>{};}
bool SaturationGraph::beginCrossoverGesture(){return state->begin();}
bool SaturationGraph::dragCrossovers(float low,float high){return state->write(low,high);}
bool SaturationGraph::endCrossoverGesture(){return state->finish();}
bool SaturationGraph::beginBandAudition(SaturationLane lane){return state->beginBand(lane);}
void SaturationGraph::endBandAudition(){state->endBand();}
void SaturationGraph::retire(){if(!state||state->retired)return;const auto keep=state;keep->retired=true;keep->unwatch();keep->cancel();keep->proposal.reset();keep->slot.reset();}
void SaturationGraph::resized(){for(std::size_t n=0;n<3;++n)state->buttons[n].setBounds(getWidth()-280+int(n)*82,132,76,24);}
void SaturationGraph::paint(juce::Graphics& g)
{
    const auto& s=*state;if(s.retired||!s.slot)return;
    const auto* model=studioSkin();const auto font=model?Lettering(model->screenFace).fontForCap(16):juce::Font(juce::FontOptions(20));g.setFont(font);
    const juce::Colour ink(0xffe5e6e1),previewInk(0xffceaf7d);const auto corners=s.preview(s.effective);const auto area=s.plot();const auto base=s.index(0)-5;
    const bool full=parameterPhysicalValue(s.effective,base)==0;
    ParameterValues candidate;const bool proposalVisible=s.proposed(candidate)&&s.marked(candidate);
    g.setColour(ink);g.drawText("CROSSOVER PREVIEW",28,2,250,24,juce::Justification::centredLeft);
    g.drawText(full?"FULL MODE / THREE-BAND RETAINED / INACTIVE":"THREE-BAND / FULL RETAINED / INACTIVE",280,2,getWidth()-308,24,juce::Justification::centredRight);
    g.drawText("RAW   LOW "+hz(s.raw(0))+"    HIGH "+hz(s.raw(1)),28,29,getWidth()/2-28,24,juce::Justification::centredLeft);
    g.setColour(previewInk);g.drawText("PREVIEW (MACROS + CLAMPS)   "+hz(corners[0])+" / "+hz(corners[1]),getWidth()/2,29,getWidth()/2-28,24,juce::Justification::centredRight);
    g.setColour(juce::Colour(0xff15191a));g.fillRect(area);
    const auto effectiveLow=s.x(corners[0]),effectiveHigh=s.x(corners[1]);
    if(!full){g.setColour(previewInk.withAlpha(.1f));g.fillRect(juce::Rectangle<float>(area.getX(),area.getY(),effectiveLow-area.getX(),area.getHeight()));g.fillRect(juce::Rectangle<float>(effectiveHigh,area.getY(),area.getRight()-effectiveHigh,area.getHeight()));}
    for(float f:{20.f,100.f,1000.f,10000.f,20000.f}){
        const auto x=s.x(f);g.setColour(ink.withAlpha(.18f));g.drawVerticalLine(juce::roundToInt(x),area.getY(),area.getBottom());
        g.setColour(ink.withAlpha(.8f));g.drawText(f==1000?"1k":f==10000?"10k":f==20000?"20k":juce::String(f,0),juce::roundToInt(x)-25,108,50,22,juce::Justification::centred);
    }
    for(int n=0;n<2;++n){const auto rawX=s.x(s.raw(n)),effectiveX=s.x(corners[std::size_t(n)]);
        g.setColour(previewInk.withAlpha(full?.55f:1.f));g.drawVerticalLine(juce::roundToInt(effectiveX),area.getY(),area.getBottom());
        g.setColour(ink);g.drawEllipse(rawX-7,area.getCentreY()-7,14,14,n==s.selected?2.5f:1.4f);
        g.drawText(n==0?"L":"H",juce::roundToInt(rawX)-7,juce::roundToInt(area.getCentreY())-9,14,18,juce::Justification::centred);
    }
    if(proposalVisible){const auto candidateCorners=s.preview(candidate);for(int n=0;n<2;++n){
        if(s.sound.controlLocks[s.index(n)])continue;
        const auto outline=[&](float frequency,juce::Colour colour){juce::Path line;const auto x=s.x(frequency);line.startNewSubPath(x,area.getY()-4);line.lineTo(x,area.getBottom()+4);ProgrammerLook::drawDashes(g,line,colour,4,3);};
        if(s.proposal->values[s.index(n)]!=s.sound.values[s.index(n)])outline(parameterPhysicalValue(s.proposal->values,s.index(n)),ink);
        if(candidateCorners[std::size_t(n)]!=corners[std::size_t(n)])outline(candidateCorners[std::size_t(n)],previewInk);
    }
    }
    juce::String footer="SATURATION | ";footer+=s.rateKnown?hz(float(s.rate))+" PROCESSING":"UNKNOWN PROCESSING RATE / 48000 Hz FALLBACK";
    for(const auto field:{8u,10u})if(parameterPhysicalValue(s.effective,base+field-1)==1){const auto raw=parameterPhysicalValue(s.effective,base+field);footer+=(field==8?" | PRE CUT ":" | POST CUT ")+hz(raw)+" > "+hz(std::min(raw,float(.45*s.rate)));}
    if(s.locked())footer+=" | LOCKED";g.setColour(ink);g.drawFittedText(footer,{28,158,getWidth()-56,22},juce::Justification::centredLeft,1,.85f);
    juce::String hold="HOLD TO AUDITION | OVERRIDES GLOBAL MIX";
    if(s.auditionBand)hold+=" | "+s.buttons[std::size_t(*s.auditionBand)-1].getButtonText()+" HELD";
    else if(!s.canAudition()){
        if(full)hold+=" | FULL MODE";
        if(parameterPhysicalValue(s.effective,572+*s.slot*23)!=0)hold+=" | SLOT BYPASSED";
        if(!s.rateKnown)hold+=" | UNPREPARED / RATE UNKNOWN";
    }
    g.drawFittedText(hold,{28,132,std::max(1,getWidth()-320),24},juce::Justification::centredLeft,1,.8f);
    if(hasKeyboardFocus(false)){g.setColour(previewInk);g.drawRect(getLocalBounds().reduced(2),1);}
}
void SaturationGraph::mouseDown(const juce::MouseEvent& e)
{
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing()||!s.slot||!e.mods.isLeftButtonDown()||!s.plot().expanded(0,10).contains(e.position))return;
    s.selected=std::abs(s.x(s.raw(0))-e.position.x)<=std::abs(s.x(s.raw(1))-e.position.x)?0:1;s.describe();
    if(s.begin()){if(getPeer())grabKeyboardFocus();repaint();}
}
void SaturationGraph::mouseDrag(const juce::MouseEvent& e)
{
    const auto keep=state;auto& s=*keep;if(s.retired||s.closing()||!s.slot){s.finish();return;}
    const auto value=std::clamp(s.frequency(e.position.x),s.selected==0?40.f:500.f,s.selected==0?2000.f:16000.f);
    s.write(s.selected==0?value:s.raw(0),s.selected==1?value:s.raw(1));
}
void SaturationGraph::mouseUp(const juce::MouseEvent&){state->cancel();}
bool SaturationGraph::keyPressed(const juce::KeyPress& k)
{
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing()||!s.slot||!s.visible())return false;
    if(k.isKeyCode(juce::KeyPress::escapeKey)){s.cancel();return true;}
    if(k.isKeyCode(juce::KeyPress::upKey)||k.isKeyCode(juce::KeyPress::downKey)){s.finish();if(s.retired||s.closing())return true;s.selected=k.isKeyCode(juce::KeyPress::upKey)?0:1;s.describe();repaint();return true;}
    const bool left=k.isKeyCode(juce::KeyPress::leftKey),right=k.isKeyCode(juce::KeyPress::rightKey);if(!left&&!right)return false;
    const auto value=s.raw(s.selected)*std::pow(2.f,(left?-1.f:1.f)*(k.getModifiers().isShiftDown()?1.f/3:1.f/24));
    s.single(std::clamp(value,s.selected==0?40.f:500.f,s.selected==0?2000.f:16000.f));return true;
}
void SaturationGraph::focusLost(FocusChangeType){const auto keep=state;keep->cancel();if(!keep->retired)repaint();}
void SaturationGraph::visibilityChanged(){const auto keep=state;if(!keep->visible())keep->cancel();}
void SaturationGraph::parentHierarchyChanged(){const auto keep=state;if(keep)keep->watch();}
std::unique_ptr<juce::AccessibilityHandler> SaturationGraph::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::slider,juce::AccessibilityActions{},juce::AccessibilityHandler::Interfaces{std::make_unique<State::Accessible>(state)});
}
}
