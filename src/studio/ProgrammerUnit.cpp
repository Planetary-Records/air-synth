#include "ProgrammerUnit.h"

#include "HouseUnit.h"
#include "InstrumentProcessor.h"
#include "ParameterListenerRetirement.h"
#include "ParameterRegistry.h"
#include "ProgrammerValues.h"
#include "SliderAccessibility.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace airack::studio
{
static float num (const juce::var& v) { return (float) (double) v; }

// The look letters a name 16 px down its label, centred across it; this width keeps the lock's dot inside.
static constexpr int labelWidth = 150, labelHeight = 24;

//============================================================================== ProgrammerKnob

ProgrammerKnob::ProgrammerKnob (InstrumentProcessor& p, const ProgrammerLook& l, house::Host& h, std::size_t parameter)
    : KeyFocus<juce::Slider> (juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox), index (parameter), processor (p), look (l), host (h),
      gesture (p, parameter, h, [this] (float physical) { read (physical); })
{
    const auto& d = parameterRegistry()[index];
    auto& param = p.control (index);
    const auto& r = param.getNormalisableRange();
    setNormalisableRange ({ r.start, r.end, r.interval, r.skew, r.symmetricSkew });
    setComponentID (d.id);
    setName (d.name);
    setTitle (d.name);
    setWantsKeyboardFocus (true);
    textFromValueFunction = [this] (double v) { return displayName() + " " + formatted ((float) v); };
    valueFromTextFunction = [this] (const String& text)
    {
        auto typed = text.trim();
        if (typed.startsWithIgnoreCase (displayName() + " "))
            typed = typed.substring (displayName().length());
        const auto v = parsed (typed, unitShown (index, (float) getValue()));
        return v ? (double) *v : getValue();
    };
    // An edit's callbacks may close the editor, which then lives until this frame ends.
    onValueChange = [this]
    {
        const auto safe = juce::Component::SafePointer<ProgrammerKnob> (this);
        const auto scope = host.frame ? host.frame() : nullptr;
        const detail::ParameterListenerDispatch dispatch(processor);
        if (onEdit)
            onEdit ((float) getValue());
        else
            gesture.write ((float) getValue());
        if (safe != nullptr && ! (safe->host.closing && safe->host.closing()))
            safe->touched();
    };

    label.setText (look.model.knobLabels[index], juce::dontSendNotification);
    label.setComponentID (d.id + ".label");
    ProgrammerLook::setRole (label, ProgrammerLook::Text::name);
    label.setInterceptsMouseClicks (false, false);
    label.setAccessible (false);

    // Hidden numeric entries need no caret timer during the editor's hierarchy changes.
    entry.setCaretVisible (false);
    entry.setComponentID (d.id + ".entry");
    entry.setCaretVisible (false);
    entry.setTitle (d.name + " value");
    entry.setFont (look.fieldFont());
    entry.setJustification (juce::Justification::centred);
    // Enter and Escape give the focus back to the knob; a click elsewhere leaves it where it went.
    entry.onReturnKey = [this]
    {
        const auto safe = juce::Component::SafePointer<ProgrammerKnob> (this);
        commitEntry();
        if (safe != nullptr && safe->isShowing())
            safe->grabKeyboardFocus();
    };
    entry.onEscapeKey = [this]
    {
        closeEntry();
        if (isShowing())
            grabKeyboardFocus();
    };
    // JUCE posts the loss: by its delivery the entry may have been reopened and focused again.
    entry.onFocusLost = [this]
    {
        if (! entry.hasKeyboardFocus (false))
            closeEntry();
    };

    attachment = std::make_unique<juce::ParameterAttachment> (param, [safe = juce::Component::SafePointer<ProgrammerKnob> (this)] (float physical) { if (safe) safe->read (physical); });
    attachment->sendInitialUpdate();
}

ProgrammerKnob::~ProgrammerKnob()
{
    closeEntry();
    gesture.end();
    onValueChange = nullptr;
    attachment.reset();
    extensionAttachment.reset();
}

void ProgrammerKnob::retire()
{
    gesture.retire();dragging=false;onValueChange=nullptr;
    detail::ParameterListenerDispatch::retire(processor,attachment);detail::ParameterListenerDispatch::retire(processor,extensionAttachment);
    onTouched=nullptr;addToMenu=nullptr;onEdit=nullptr;closeEntry();setEnabled(false);
}

std::unique_ptr<juce::AccessibilityHandler> ProgrammerKnob::createAccessibilityHandler()
{
    return std::make_unique<detail::SliderAccessibility> (*this, processor, [this] { return host.frame ? host.frame() : nullptr; });
}

const std::vector<String>& ProgrammerKnob::choiceValues() const
{
    static const std::vector<String> shapes {"Bell","Low shelf","High shelf","Low cut","High cut","Notch","Band pass","Tilt shelf","Flat tilt"};
    static const std::vector<String> slopes {"6 dB/oct","12 dB/oct","24 dB/oct","36 dB/oct","48 dB/oct","96 dB/oct"};
    return eqExtension ? (eqShape ? shapes : slopes) : parameterRegistry()[index].choices;
}
String ProgrammerKnob::formatted (float value) const
{
    if (! eqExtension) return formatParameter (index, value);
    const auto& names=choiceValues();
    return names[(std::size_t) juce::jlimit (0, (int) names.size()-1, (int) std::lround (value))];
}
std::optional<float> ProgrammerKnob::parsed (const String& text, const String& unit) const
{
    if (! eqExtension) return parseParameter (index, text, unit);
    const auto typed=text.trim();const auto& names=choiceValues();
    for(std::size_t n=0;n<names.size();++n)
        if(typed.equalsIgnoreCase(names[n]) || (!eqShape && typed==names[n].upToFirstOccurrenceOf(" ",false,false)))return float(n);
    for(std::size_t n=0;n<names.size();++n)if(typed==String(int(n)))return float(n);
    return {};
}
float ProgrammerKnob::eqSelection() const
{
    const auto classic=int(std::lround(processor.control(index).convertFrom0to1(processor.control(index).getValue())));
    const auto extra=int(std::lround(processor.control(*eqExtension).convertFrom0to1(processor.control(*eqExtension).getValue())));
    if(eqShape)return float(extra?extra+5:classic);
    return float(extra?(extra==1?0:extra+1):classic+1);
}
std::optional<ProgrammerKnob::ChoiceContext> ProgrammerKnob::captureChoice() const
{
    ChoiceContext context;
    if(!processor.trySoundSnapshot(context.sound,context.revision,context.reset))return {};
    return context;
}
void ProgrammerKnob::useEqSelector (std::size_t extension)
{
    eqExtension=extension;eqShape=(index-dynamicEqParameterStart)%dynamicEqParametersPerSlot%dynamicEqParametersPerBand==2;
    setRange(0,eqShape?8:5,1);
    onEdit=[this](float value){writeEqChoice(int(std::lround(value)),captureChoice());};
    extensionAttachment=std::make_unique<juce::ParameterAttachment>(processor.control(extension),[this](float){reread();});
    reread();
}
void ProgrammerKnob::writeEqChoice (int selected, const std::optional<ChoiceContext>& expected)
{
    const auto safe=juce::Component::SafePointer<ProgrammerKnob>(this);
    auto& backend=processor;const auto leader=index;const auto extra=*eqExtension;
    const auto scope=host.frame?host.frame():nullptr;
    const detail::ParameterListenerDispatch dispatch(processor);
    if(safe==nullptr)return;
    if(!expected || (host.closing&&host.closing()) || selected<0 || selected>=int(choiceValues().size())){gesture.reject();return;}
    std::array<ParameterGestureValue,2> edits{{{leader,expected->sound.values[leader]},{extra,0}}};
    if(eqShape){
        if(selected<6)edits[0].value=backend.control(leader).convertTo0to1(float(selected));
        else edits[1].value=backend.control(extra).convertTo0to1(float(selected-5));
    }else{
        if(selected==1||selected==2)edits[0].value=backend.control(leader).convertTo0to1(float(selected-1));
        else edits[1].value=backend.control(extra).convertTo0to1(float(selected==0?1:selected-1));
    }
    const auto slot=(leader-dynamicEqParameterStart)/dynamicEqParametersPerSlot;
    bool locked=expected->sound.effectLocks[slot];
    for(const auto& edit:edits)locked|=expected->sound.controlLocks[edit.index]&&edit.value!=expected->sound.values[edit.index];
    if(locked || !backend.beginParameterGesture(leader,edits,expected->sound,expected->revision,expected->reset)){if(safe!=nullptr)safe->gesture.reject();return;}
    const bool written=safe!=nullptr && !(safe->host.closing&&safe->host.closing())
        && backend.setParameterGestureValues(leader,edits,expected->sound,expected->revision,expected->reset);
    backend.endParameterGesture(leader);
    if(safe!=nullptr){safe->reread();if(!written)safe->gesture.reject();}
}

void showDynamicEqRelevance(ProgrammerKnob& knob,const ParameterValues& values)
{
    const auto index=knob.index;if(index<dynamicEqParameterStart||index>=dynamicEqGainParameterStart)return;
    const auto local=(index-dynamicEqParameterStart)%dynamicEqParametersPerSlot;if(local==88)return;
    const auto field=local%dynamicEqParametersPerBand;
    auto shape=int(std::lround(parameterPhysicalValue(values,index-field+2)));
    const auto extension=dynamicEqExtensionParameterStart+(index-dynamicEqParameterStart)/dynamicEqParametersPerSlot*dynamicEqExtensionParametersPerSlot
        +local/dynamicEqParametersPerBand*dynamicEqExtensionParametersPerBand;
    const auto extra=int(parameterPhysicalValue(values,extension));if(extra)shape=extra+5;
    const bool cut=shape==3||shape==4,dynamic=shape<=2,gain=dynamic||shape==7||shape==8;
    const bool q=dynamic||shape==5||shape==6||shape==7;
    const bool relevant=field==6?cut:field==5?q:field==4?gain:field>=7?dynamic:true;
    knob.setEnabled(shape<6||relevant);
    const auto alpha=relevant?1.f:.4f;knob.setAlpha(alpha);knob.label.setAlpha(alpha);
    knob.setDescription(relevant?String():shape<6?String("Value retained; unused by this filter shape. Editable for later shape changes.")
        :String("Value retained; unused by this static filter shape."));
}

void showSaturationRelevance(ProgrammerKnob& knob,const SoundDocument& document)
{
    const auto& values=document.values;
    if(knob.index>=572&&knob.index<756&&(knob.index-572)%23==0){
        knob.label.setText("BYPASS",juce::dontSendNotification);
        knob.setTitle(parameterRegistry()[knob.index].name);
        knob.setDescription(String());
        return;
    }
    const auto id=juce::String(parameterRegistry()[knob.index].id);
    if(!id.startsWith("effect.")||!id.contains(".saturation."))return;
    const auto slot=id.fromFirstOccurrenceOf("effect.",false,false).upToFirstOccurrenceOf(".",false,false).getIntValue()-1;
    if(!juce::isPositiveAndBelow(slot,8))return;
    static const auto states=[]{
        std::array<std::array<std::size_t,3>,8> result{};
        for(std::uint8_t bank=0;bank<8;++bank){int field=0;for(const auto* key:{"saturation.mode","saturation.pre_low_cut_enabled","saturation.post_high_cut_enabled"})
            result[bank][std::size_t(field++)]=rackParameterIndex(NativeModule::saturation,bank,key).value_or(parameterCount);}
        return result;
    }();
    const auto& state=states[std::size_t(slot)];if(state[0]>=parameterCount||state[1]>=parameterCount||state[2]>=parameterCount)return;
    const auto key=id.fromFirstOccurrenceOf(".saturation.",false,false);
    const bool three=parameterPhysicalValue(values,state[0])>.5f;
    bool relevant=key.startsWith("full.")?!three:(key.startsWith("low.")||key.startsWith("mid.")||key.startsWith("high.")||key=="low_crossover_hz"||key=="high_crossover_hz")?three:true;
    if(key=="pre_low_cut_hz")relevant=parameterPhysicalValue(values,state[1])>.5f;
    if(key=="post_high_cut_hz")relevant=parameterPhysicalValue(values,state[2])>.5f;
    knob.setAlpha(relevant?1.f:.8f);knob.label.setAlpha(relevant?1.f:.8f);
    knob.setDescription(relevant?String():String("Value retained; unused by current saturation settings. Editable for later changes."));
}

bool showSpaceReverbRelevance(ProgrammerKnob& knob,const ParameterValues& values)
{
    const auto index=knob.index;
    if(index<spaceReverbParameterStart||index>=spaceReverbParameterStart+8*spaceReverbParametersPerSlot)return true;
    const auto local=(index-spaceReverbParameterStart)%spaceReverbParametersPerSlot;
    if(local<48)return true;
    const auto field=(local-48)%8;
    const auto shape=int(std::lround(parameterPhysicalValue(values,index-field+2)));
    const bool cut=shape==3||shape==4;
    const bool relevant=field==4?!cut:field==5?shape==0:field==6?(shape==1||shape==2):field==7?cut:true;
    const juce::Component::SafePointer<ProgrammerKnob> alive(&knob);
    // JUCE's setEnabled resumes on this component after its enablement callback.
    if(!relevant&&!knob.finishDrag())return false;
    knob.setEnabled(relevant);if(!alive)return false;
    knob.label.setEnabled(relevant);knob.entry.setEnabled(relevant);
    const auto alpha=relevant?1.f:.4f;knob.setAlpha(alpha);knob.label.setAlpha(alpha);
    knob.setDescription(relevant?String():String("Value retained; unused by the current wet-EQ shape."));
    return true;
}

void ProgrammerKnob::place (Pt c)
{
    middle = c;
    const auto K = look.model.programmer["knob"];
    const auto side = look.model.asset (K["body"].toString()).image.getWidth();
    setBounds (juce::roundToInt (c.x - (float) side * 0.5f), juce::roundToInt (c.y - (float) side * 0.5f), side, side);
    const auto baseline = c.y + num (K["label_dy"]);
    // An odd width puts the label's middle, where the look centres the name, on a half pixel: the nearer of the two
    // keeps the name within a quarter pixel of the knob's centre.
    const auto doubled = juce::roundToInt (c.x * 2.0f), w = labelWidth + (doubled & 1);
    label.setBounds ((doubled - w) / 2, juce::roundToInt (baseline) - 16, w, labelHeight);
    entry.setBounds (Rect (96.0f, 22.0f).withCentre ({ c.x, baseline - look.labelStyle.cap * 0.5f }).toNearestInt());
}

void ProgrammerKnob::paint (juce::Graphics& g)
{
    if(equipment!=nullptr)
    {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        const auto centre=getLocalBounds().toFloat().getCentre();
        if(presentation==Presentation::rotary){
            const auto scale=(float)getWidth()/104.f;
            for(const auto* key:{"advanced:knob-ring","advanced:knob"}){const auto& a=equipment->asset(key);
                g.drawImageTransformed(a.image,juce::AffineTransform::translation(-a.anchor.x,-a.anchor.y).rotated(String(key)=="advanced:knob"?juce::degreesToRadians(angle()):0.f).scaled(scale).translated(centre.x,centre.y));}
        }else house::paintNineSlice(g,equipment->asset("advanced:key"),getLocalBounds());
        if(ringed()){g.setColour(accent);g.drawRect(getLocalBounds().reduced(1),2);}
        return;
    }
    if (presentation == Presentation::rotary)
        return juce::Slider::paint (g);
    const auto area = getLocalBounds().toFloat();
    if (! isEnabled()) g.setOpacity (.45f);
    if(presentation==Presentation::horizontal){
        g.setColour(juce::Colour(0xffa5eeee));g.setFont(look.mono.fontForCap(25));
        g.drawFittedText(formatted(float(getValue())),getLocalBounds().withHeight(43),juce::Justification::centred,1);
        const float start=12,end=area.getWidth()-12,y=area.getHeight()-14,x=start+float(proportion())*(end-start);
        g.setColour(juce::Colour(0xff526268));g.drawLine(start,y,end,y,3);
        g.setColour(juce::Colour(0xff8cdbd9));g.drawLine(start,y,x,y,3);g.fillRect(Rect(x-6,y-7,12,14));
        if(ringed()){g.setColour(juce::Colour(0xff8bc7ae));g.drawRect(getLocalBounds());}
        return;
    }
    look.drawGlass (g, area);
    if (ringed()) { g.setColour (juce::Colour (0xff8bc7ae)); g.drawRect (getLocalBounds()); }
    g.setFont (look.caps.fontForCap (float(getProperties().getWithDefault("programmer.choiceCap",18.f))));
    if (presentation == Presentation::toggle)
    {
        if(bool(getProperties().getWithDefault("programmer.digitalToggle",false))){
            g.setColour(getValue()!=0?juce::Colour(0xffa5eeee):juce::Colour(0xff66716a));
            g.drawText(getValue()!=0?"ON":"OFF",getLocalBounds().withTrimmedLeft(getWidth()*2/3),juce::Justification::centred);return;
        }
        g.setColour (getValue() != 0 ? juce::Colour (0xffd1efe0) : juce::Colour (0xff66716a));
        const bool softKey=bool(getProperties().getWithDefault("programmer.softKey",false));
        g.fillEllipse (softKey?area.getRight()-28:9, area.getCentreY()-(softKey?4:3), softKey?8:6, softKey?8:6);
        return;
    }
    if (presentation == Presentation::segments)
    {
        const auto& names=choiceValues();const auto count=(int)names.size();
        const bool placement=index>=dynamicEqPlacementParameterStart&&index<delayTapeParameterStart;
        const char* placementNames[] { "Stereo", "L", "R", "M", "S" };
        for (int n = 0; n < count; ++n)
        {
            const auto cell = Rect (area.getWidth() * n / count, 0, area.getWidth() / count, area.getHeight());
            if (n == (int) std::lround (getValue()))
            {
                g.setColour (juce::Colour (0xff344b42)); g.fillRect (cell.reduced (1));
                g.setColour (juce::Colour (0xff8bc7ae)); g.fillRect (cell.withTop (cell.getBottom() - 2));
            }
            g.setColour (juce::Colour (0xffceded5));
            if(placement)g.drawText(placementNames[n],cell.toNearestInt(),juce::Justification::centred);
            else g.drawFittedText(names[std::size_t(n)],cell.reduced(6,0).toNearestInt(),juce::Justification::centred,1,.8f);
            if (n > 0) { g.setColour (juce::Colour (0xff4a514b)); g.drawVerticalLine (juce::roundToInt (cell.getX()), 2, area.getHeight() - 2); }
        }
        return;
    }
    g.setColour (juce::Colour (0xffd3dfd7));
    const auto prefix=index>=dynamicEqSpectralParameterStart&&index<spaceReverbParameterStart&&(index-dynamicEqSpectralParameterStart)%25==0?String("Requested: "):String();
    g.drawFittedText (prefix+formatted ((float) getValue()), getLocalBounds().reduced (10, 0).withTrimmedRight (12),
                     juce::Justification::centredLeft, 1, 0.8f);
    juce::Path arrow; const auto x = area.getRight() - 14, y = area.getCentreY();
    arrow.addTriangle (x - 3, y - 1, x + 3, y - 1, x, y + 2); g.fillPath (arrow);
}

float ProgrammerKnob::angle() const { return angleAt (getValue()); }

float ProgrammerKnob::angleAt (double physical) const
{
    return (float) ((2.0 * getNormalisableRange().convertTo0to1 (physical) - 1.0) * num (look.model.programmer["knob"]["sweep"][1]));
}

int ProgrammerKnob::steps() const
{
    if(eqExtension)return int(choiceValues().size())-1;
    const auto& d = parameterRegistry()[index];
    return d.kind == ParameterKind::continuous ? 0 : (int) d.choices.size() - 1;
}

String ProgrammerKnob::valueLine() const { return displayName() + " " + formatted ((float) getValue()); }

void ProgrammerKnob::setLocked (bool on)
{
    locked = on;
    ProgrammerLook::setLocked (label, on);
}

Rect ProgrammerKnob::labelInk() const
{
    const auto w = (float) label.getWidth();
    auto style=look.labelStyle;if(equipment)style.cap=14;
    return setText (look.faces, style, label.getText().toUpperCase(), 0.0f, 16.0f, w, 0.8f, w * 0.5f).ink() + label.getPosition().toFloat();
}

Rect ProgrammerKnob::lockArea() const
{
    const auto ink = labelInk();
    const auto lk = look.model.programmer["lock"];
    const auto r = num (look.model.asset (lk["part"].toString()).spec["r"]);
    return Rect (2.0f * r, 2.0f * r).withCentre ({ ink.getX() + num (lk["dx"]), ink.getCentreY() + num (lk["dy"]) });
}

void ProgrammerKnob::reread() { if(attachment)attachment->sendInitialUpdate(); }

ProgrammerKnob::Menu ProgrammerKnob::menu()
{
    Menu m;
    m.push_back ({ locked ? "Unlock" : "Lock", [this, on = ! locked]
                   {
                       const auto scope = host.frame ? host.frame() : nullptr;
                       const detail::ParameterListenerDispatch dispatch(processor);
                       if (host.change && ! (host.closing && host.closing()))
                           host.change ([i = index, extra=eqExtension, on] (SoundDocument& s) { s.controlLocks[i] = on;if(extra)s.controlLocks[*extra]=on; }, "Control lock");
                   } });
    if(eqExtension)m.push_back({"Default",[this,expected=captureChoice()]{writeEqChoice(eqShape?0:1,expected);}});
    else m.push_back ({ "Default", [this] { setValue (parameterRegistry()[index].defaultValue, juce::sendNotificationSync); } });
    if (addToMenu)
        addToMenu (m);
    return m;
}

void ProgrammerKnob::showMenu()
{
    auto items = menu();
    juce::PopupMenu pm;
    for (size_t i = 0; i < items.size(); ++i)
        pm.addItem ((int) i + 1, items[i].first);
    pm.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                      [safe = juce::Component::SafePointer<ProgrammerKnob> (this), items] (int chosen)
                      {
                          if (safe != nullptr && chosen > 0)
                              items[(size_t) chosen - 1].second();
                      });
}

void ProgrammerKnob::openEntry()
{
    if (! isEnabled() || dragging)
        return;
    entryUnit = unitShown (index, (float) getValue());
    entry.setText (formatted ((float) getValue()), false);
    entry.setCaretVisible (true);
    entry.setVisible (true);
    entry.toFront (false);
    if (entry.isShowing())
        entry.grabKeyboardFocus();
    entry.selectAll();
}

void ProgrammerKnob::commitEntry()
{
    if (! entry.isVisible())
        return;
    const auto text = entry.getText();
    const juce::Component::SafePointer<ProgrammerKnob> alive(this);
    closeEntry();
    if(!alive)return;
    if (const auto v = parsed (text, entryUnit))
        setValue (*v, juce::sendNotificationSync);
}

void ProgrammerKnob::closeEntry()
{
    entry.setCaretVisible (false);
    if (entry.isVisible())
        entry.setVisible (false);
}

void ProgrammerKnob::move (double f)
{
    f = juce::jlimit (0.0, 1.0, f);
    const auto n = steps();
    const auto now = proportion();
    if (n > 0)
    {
        const auto step = std::lround (f * n);
        if (step != std::lround (now * n))
            setValue (getNormalisableRange().convertFrom0to1 ((double) step / n), juce::sendNotificationSync);
    }
    else if (std::abs (f - now) > 1.0e-6)
        setValue (getNormalisableRange().convertFrom0to1 (f), juce::sendNotificationSync);
}

void ProgrammerKnob::stepBy (int k)
{
    const auto n = steps();
    move (n > 0 ? (double) (std::lround (proportion() * n) + k) / n : proportion() + k / 100.0);
}

void ProgrammerKnob::showChoices()
{
    juce::PopupMenu choices; const auto& values = choiceValues();
    for (std::size_t n = 0; n < values.size(); ++n) choices.addItem ((int) n + 1, values[n], true, (int) std::lround (getValue()) == (int) n);
    choices.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [safe = juce::Component::SafePointer<ProgrammerKnob> (this),expected=eqExtension?captureChoice():std::optional<ChoiceContext>{}] (int selected)
        { if (safe != nullptr && selected > 0){if(safe->eqExtension){safe->writeEqChoice(selected-1,expected);if(safe!=nullptr)safe->touched();}
            else safe->move ((double) (selected - 1) / std::max (1, safe->steps()));} });
}

void ProgrammerKnob::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled())
        return;
    if (e.mods.isPopupMenu())
        return showMenu();
    const auto scope = host.frame ? host.frame() : nullptr;
    const detail::ParameterListenerDispatch dispatch(processor);
    if (presentation == Presentation::toggle) return move (1.0 - proportion());
    if (presentation == Presentation::segments)
    {const auto n=steps();if(n>0)move(juce::jlimit(0,n,(int)(e.position.x*(n+1)/(float)getWidth()))/(double)n);return;}
    if (presentation == Presentation::choice) return showChoices();

    if (dragging || (! onEdit && ! gesture.begin()))
        return;
    dragging = true;
    dragFrom = proportion();
    dragStart = getValue();
    touched();
    if (isShowing())
        grabKeyboardFocus();
}

void ProgrammerKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        move (dragFrom + (presentation==Presentation::horizontal?e.position.x-e.mouseDownPosition.x:e.mouseDownPosition.y-e.position.y) / (e.mods.isShiftDown() ? 1000.0 : presentation==Presentation::horizontal?std::max(1,getWidth()-24):200.0));
}

void ProgrammerKnob::mouseUp (const juce::MouseEvent&)
{
    const detail::ParameterListenerDispatch dispatch(processor);
    if (! dragging)
        return;
    dragging = false;
    gesture.end();
}

void ProgrammerKnob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        openEntry();
}

void ProgrammerKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (rackScrolling())
        return juce::Component::mouseWheelMove (e, w);
    if (isEnabled() && ! dragging && w.deltaY != 0.0f)
        stepBy (w.deltaY > 0.0f ? 1 : -1);
}

bool ProgrammerKnob::keyPressed (const juce::KeyPress& key)
{
    const detail::ParameterListenerDispatch dispatch(processor);
    if (! isEnabled())
        return false;
    const auto code = key.getKeyCode();
    if (dragging)
    {
        if (code != juce::KeyPress::escapeKey)
            return false;
        setValue (dragStart, juce::sendNotificationSync);
        dragging = false;
        gesture.end();
        return true;
    }
    if (code == juce::KeyPress::spaceKey && presentation == Presentation::toggle) { move (1.0 - proportion()); return true; }
    if (code == juce::KeyPress::spaceKey && presentation == Presentation::choice) { showChoices(); return true; }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey) stepBy (1);
    else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey) stepBy (-1);
    else if (code == juce::KeyPress::pageUpKey) stepBy (10);
    else if (code == juce::KeyPress::pageDownKey) stepBy (-10);
    else if (code == juce::KeyPress::homeKey) move (0.0);
    else if (code == juce::KeyPress::endKey) move (1.0);
    else if (code == juce::KeyPress::returnKey) openEntry();
    else return false;
    return true;
}

bool ProgrammerKnob::finishDrag()
{
    const detail::ParameterListenerDispatch dispatch(processor);
    const juce::Component::SafePointer<ProgrammerKnob> alive(this);
    KeyFocus<juce::Slider>::enablementChanged();
    if(!alive)return false;
    closeEntry();
    if(!alive)return false;
    if (dragging)
    {
        dragging = false;
        // Removing this listener inside a host End callback would shift JUCE's active listener iteration.
        auto registered = std::move(attachment);
        gesture.end();
        if(!alive)return false;
        attachment = std::move(registered);
    }
    return true;
}

void ProgrammerKnob::enablementChanged()
{
    const juce::Component::SafePointer<ProgrammerKnob> alive(this);
    KeyFocus<juce::Slider>::enablementChanged();
    if(!alive||!finishDrag())return;
    if (! isEnabled())
        closeEntry();
}

bool ProgrammerKnob::hitTest (int x, int y)
{
    if (presentation != Presentation::rotary) return getLocalBounds().contains (x, y);
    return Pt ((float) x, (float) y).getDistanceFrom ({ (float) getWidth() * 0.5f, (float) getHeight() * 0.5f })
           <= num (look.model.programmer["knob"]["hit_r"]);
}

String ProgrammerKnob::displayName() const
{
    const auto& shown = look.model.knobNames[index];
    return shown.isNotEmpty() ? shown : String (parameterRegistry()[index].name).toUpperCase();
}

double ProgrammerKnob::proportion() const { return getNormalisableRange().convertTo0to1 (getValue()); }

void ProgrammerKnob::read (float physical) { setValue (eqExtension?eqSelection():physical, juce::dontSendNotification); }

void ProgrammerKnob::touched()
{
    if (onTouched)
        onTouched (*this);
}

//============================================================================== ProgrammerUnit

namespace
{
// The effect kind whose name in capitals is the title, else -1.
int kindTitled (const String& title)
{
    for (int k = 0; k <= (int) EffectKind::saturation; ++k)
        if (k != (int) EffectKind::spaceReverb && effectDisplayName ((EffectKind) k).toUpperCase() == title)
            return k;
    return -1;
}

juce::Colour grey (int level) { return juce::Colour::greyLevel ((float) level / 255.0f); }
juce::Colour fromBgr (const juce::var& v) { return juce::Colour ((juce::uint8) (int) v[2], (juce::uint8) (int) v[1], (juce::uint8) (int) v[0]); }

house::Assignment assignmentOf (const MacroBinding& b) { return { parameterIndexForKey (b.targetKey), b.amount }; }

bool same (const house::Assignment& a, const house::Assignment& b) { return a.target == b.target && std::abs (a.amount - b.amount) < 0.0005f; }

/** What today's macro target list offers. */
bool macroTarget (std::size_t index) { return parameterRegistry()[index].kind == ParameterKind::continuous && (index == 0 || index >= 9); }

template <std::size_t N>
int position (const std::array<std::uint8_t, N>& order, std::size_t slot)
{
    return (int) (std::find (order.begin(), order.end(), (std::uint8_t) slot) - order.begin()) + 1;
}

/** Today's RackRow sentence, word for word. */
String rowSentence (const SoundDocument& d, bool synth, std::size_t n)
{
    const auto use = synth ? d.layout.synthUse[n] : d.layout.effects[n].use;
    auto text = (synth ? "Synth " : "Effect ") + String ((int) n + 1) + " physical bank / ";
    text += use == SlotUse::present ? "Present" : use == SlotUse::retired ? "Retired - Replace to reuse" : "Unused";
    if (use != SlotUse::present)
        return text;
    if (synth)
        return text + " / position " + String (position (d.layout.synthOrder, n));
    const auto chain = d.layout.effects[n].chain;
    return text + " / " + (chain == masterChain ? String ("Master") : "Synth " + String (chain + 1)) + " / position "
           + String (position (d.layout.effectOrder[chain], n));
}

/** The row sentence in the PROGRAMMER's form, in capitals with its fields two spaces apart. */
String rowText (const SoundDocument& d, bool synth, std::size_t n)
{
    const auto use = synth ? d.layout.synthUse[n] : d.layout.effects[n].use;
    const auto head = (synth ? "SYNTH " : "EFFECT ") + String ((int) n + 1) + "  ";
    if (use != SlotUse::present)
        return head + (use == SlotUse::retired ? "RETIRED  REPLACE TO REUSE" : "UNUSED");
    if (synth)
        return head + "PRESENT  POSITION " + String (position (d.layout.synthOrder, n));
    const auto chain = d.layout.effects[n].chain;
    return head + "PRESENT  " + (chain == masterChain ? String ("MASTER") : "SYNTH " + String (chain + 1)) + "  POSITION "
           + String (position (d.layout.effectOrder[chain], n));
}

const RackUnit* unitOf (const RackInstance& i, std::uint8_t id)
{
    const auto u = std::find_if (i.design.units.begin(), i.design.units.end(), [id] (const auto& v) { return v.id == id; });
    return u == i.design.units.end() ? nullptr : &*u;
}
} // namespace

int pageOfKind (const SubpageSpec& fx, EffectKind kind)
{
    const auto title = effectDisplayName (kind).toUpperCase();
    for (std::size_t p = 0; p < fx.routes.size(); ++p)
        for (const auto& row : fx.routes[p].rows)
            for (const auto& s : row)
                if (s.title == title)
                    return (int) p;
    return 0;
}

void editRoute (InstrumentProcessor& processor, const ProgrammerHost& host, std::size_t index, float value, const std::function<void()>& reread)
{
    const auto scope = host.house.frame ? host.house.frame() : nullptr;
    const auto closing = [&host] { return host.house.closing && host.house.closing(); };
    if (closing())
        return;
    SoundDocument d;
    std::uint64_t revision = 0;
    if (! host.snapshot || ! host.snapshot (d, revision))
        return reread();
    const auto before = d.values;
    const auto base = 252 + ((index - 252) / 5) * 5, field = (index - 252) % 5;
    d.values[index] = value;
    int target = static_cast<int> (std::round (d.values[base + 2] * 16)), bank = static_cast<int> (std::round (d.values[base + 3] * 2));
    if (field == 3)
    {
        if (bank == 0 && target > 2) target = 1;
        else if (bank == 1 && (target <= 2 || target == 11 || target == 12)) target = 14;
        else if (bank == 2 && (target <= 2 || target == 9 || target == 10)) target = 14;
    }
    else
    {
        if (target <= 2) bank = 0;
        else if (target == 9 || target == 10) bank = 1;
        else if (target == 11 || target == 12) bank = 2;
        else if (bank == 0) bank = 1;
    }
    if (field == 0 && value >= .5f)
    {
        if (d.values[base + 1] == 0) d.values[base + 1] = .5f;
        if (target == 0) { target = 1; bank = 0; }
    }
    if (target == 0 || d.values[base + 1] == 0)
        d.values[base] = 0;
    d.values[base + 2] = static_cast<float> (target) / 16;
    d.values[base + 3] = static_cast<float> (bank) / 2;
    std::array<ParameterGestureValue, 5> edits {};
    std::size_t count = 0;
    for (std::size_t n = 0; n < 5; ++n)
        if (base + n == index || d.values[base + n] != before[base + n])
            edits[count++] = { base + n, d.values[base + n] };
    const auto began = processor.beginParameterGesture (index);
    const auto applied = began && ! closing() && processor.setParameterGestureValues (index, std::span<const ParameterGestureValue> (edits.data(), count));
    const auto ended = ! began || processor.endParameterGesture (index);
    if (closing())
        return;
    if (host.house.report)
        host.house.report (String ("Route edit") + (applied && ended ? " complete" : " failed; sound preserved"));
    if (host.refresh)
        host.refresh();
    reread();
}

/** A RACK LAYOUT row: its bank's line, on the display's glass while the bank is selected. A press selects the bank. */
class ProgrammerUnit::BankRow final : public KeyFocus<juce::ToggleButton>
{
public:
    /** The line's baseline, down the row. */
    static constexpr float baseline = 21.0f;

    BankRow (const ProgrammerLook& l, float x) : look (l), textX (x)
    {
        setClickingTogglesState (false);
        setToggleable (true);
    }

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        const auto selected = getToggleState();
        if (selected)
            look.drawGlass (g, getLocalBounds().toFloat());
        g.setColour (selected ? look.ink : grey (look.labelStyle.grey));
        g.fillPath (setText (look.faces, look.labelStyle, getButtonText().toUpperCase(), textX, baseline, (float) getWidth() - textX, 0.8f).path);
        if (ProgrammerLook::ringed (*this))
        {
            g.setColour (accent);
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 2.0f);
        }
    }

private:
    const ProgrammerLook& look;
    const float textX;
};

/** A bank's controls, today's row's: ADD, REMOVE, REPLACE, UP, DOWN and LOCK, and for an effect bank MOVE, with KIND
    and OWNER holding the choice ADD, REPLACE and MOVE take. Plate-sized, its controls in the strip under the synth
    banks: the fields over the keys, today's actions from the left at the sub-pages' pitch and LOCK at the right. */
class ProgrammerUnit::BankControls final : public juce::Component
{
public:
    BankControls (ProgrammerUnit& u, bool isSynth, int n, Rect strip) : unit (u), synth (isSynth), index (n)
    {
        setInterceptsMouseClicks (false, true);
        setSize (u.getWidth(), u.getHeight());
        const auto prefix = (synth ? "synth." : "effect.") + String (n + 1);
        const std::pair<juce::Button*, const char*> named[] { { &add, "add" }, { &remove, "remove" }, { &replace, "replace" }, { &up, "up" },
                                                              { &down, "down" }, { &move, "move" }, { &lock, "lock" } };
        for (const auto& [b, name] : named)
        {
            b->setComponentID (prefix + "." + name);
            b->setName (prefix + " " + name);
        }
        kind.setComponentID (prefix + ".kind");
        kind.setName ("Replacement effect type");
        kind.setTitle ("Kind");
        for (int k = 0; k <= (int) EffectKind::saturation; ++k)
            if (k != (int) EffectKind::spaceReverb)
                kind.addItem (effectDisplayName ((EffectKind) k), k + 1);
        kind.setSelectedId (1, juce::dontSendNotification);
        owner.setComponentID (prefix + ".owner");
        owner.setName ("Destination chain");
        owner.setTitle ("Owner");
        owner.addItemList ({ "Synth 1 insert", "Synth 2 insert", "Synth 3 insert", "Synth 4 insert", "Master" }, 1);
        owner.setSelectedId (5, juce::dontSendNotification);
        ProgrammerLook::setLegend (kind, "Kind");
        ProgrammerLook::setLegend (owner, "Owner");

        // A key at 1x is 84 x 96 and a field 40 high, its legend 8 px over it, as the rack panel draws them.
        constexpr int keyWidth = 84, keyHeight = 96, fieldHeight = 40;
        const auto pitch = num (u.model.programmer["subpages"]["pitch"]);
        const auto keysTop = juce::roundToInt (strip.getBottom()) - 32 - keyHeight;
        std::vector<juce::Component*> shown;
        if (! synth)
        {
            kind.setBounds (juce::roundToInt (strip.getX()), keysTop - 8 - fieldHeight, 200, fieldHeight);
            owner.setBounds (kind.getRight() + 24, kind.getY(), 220, fieldHeight);
            shown = { &kind, &owner };
        }
        std::vector<juce::Button*> actions { &add, &remove, &replace, &up, &down };
        if (! synth)
            actions.push_back (&move);
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            actions[i]->setBounds (juce::roundToInt (strip.getX() + pitch * (float) i), keysTop, keyWidth, keyHeight);
            shown.push_back (actions[i]);
        }
        lock.setBounds (juce::roundToInt (strip.getRight()) - keyWidth, keysTop, keyWidth, keyHeight);
        shown.push_back (&lock);
        for (std::size_t i = 0; i < shown.size(); ++i)
        {
            shown[i]->setExplicitFocusOrder ((int) i + 1);
            addAndMakeVisible (shown[i]);
        }

        add.onClick = [this] { act (synth ? RackEditKind::addSynth : RackEditKind::addEffect, false, 0); };
        replace.onClick = [this] { act (synth ? RackEditKind::addSynth : RackEditKind::replaceEffect, true, 0); };
        remove.onClick = [this] { act (synth ? RackEditKind::removeSynth : RackEditKind::removeEffect, false, 0); };
        up.onClick = [this] { act (synth ? RackEditKind::moveSynth : RackEditKind::moveEffect, false, -1); };
        down.onClick = [this] { act (synth ? RackEditKind::moveSynth : RackEditKind::moveEffect, false, 1); };
        move.onClick = [this] { act (RackEditKind::moveEffect, false, 0); };
        lock.onClick = [this]
        {
            const auto scope = unit.frame();
            if (unit.closing() || ! unit.host.house.change)
                return;
            const auto n = (std::size_t) index;
            unit.host.house.change ([s = synth, n, on = lock.getToggleState()] (SoundDocument& d) { (s ? d.synthLocks[n] : d.effectLocks[n]) = on; },
                                    "Rack lock");
        };
    }

    /** LOCK as the document holds it; KIND and OWNER too when `force`, else they keep a pending choice. */
    void refresh (const SoundDocument& d, bool force)
    {
        const auto n = (std::size_t) index;
        lock.setToggleState (synth ? d.synthLocks[n] : d.effectLocks[n], juce::dontSendNotification);
        if (synth || ! force)
            return;
        kind.setSelectedId ((int) d.layout.effects[n].kind + 1, juce::dontSendNotification);
        owner.setSelectedId (d.layout.effects[n].chain + 1, juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        if (synth)
            return;
        unit.look.drawLegend (g, kind);
        unit.look.drawLegend (g, owner);
    }

    KeyFocus<juce::TextButton> add { "Add" }, remove { "Remove" }, replace { "Replace" }, up { "Up" }, down { "Down" }, move { "Move" };
    KeyFocus<juce::ToggleButton> lock { "Lock" };
    KeyFocus<juce::ComboBox> kind, owner;

private:
    /** Today's row's edit: UP and DOWN step the bank's position; ADD, REPLACE and MOVE put it last, an effect bank in
        OWNER's chain. REPLACE adds an effect bank no rack uses. */
    void act (RackEditKind type, bool reuse, int delta)
    {
        const auto scope = unit.frame();
        if (unit.closing())
            return;
        SoundDocument d;
        std::uint64_t revision = 0;
        if (! unit.host.snapshot || ! unit.host.snapshot (d, revision))
            return;
        const auto& l = d.layout;
        if (type == RackEditKind::replaceEffect && l.effects[(std::size_t) index].use != SlotUse::present)
            type = RackEditKind::addEffect;
        RackEdit edit;
        edit.kind = type;
        edit.slot = (std::uint8_t) index;
        edit.allowReuse = reuse;
        edit.effect = (EffectKind) (kind.getSelectedId() - 1);
        edit.destination = (std::uint8_t) (owner.getSelectedId() - 1);
        if (synth)
        {
            edit.position = l.synthCount;
            if (type == RackEditKind::moveSynth)
            {
                const auto at = (int) (std::find (l.synthOrder.begin(), l.synthOrder.end(), edit.slot) - l.synthOrder.begin());
                edit.position = (std::uint8_t) juce::jlimit (0, std::max (0, (int) l.synthCount - 1), at + delta);
            }
        }
        else
        {
            const auto chain = l.effects[(std::size_t) index].chain;
            if (delta != 0)
                edit.destination = chain;
            edit.position = l.effectCount[edit.destination];
            if (type == RackEditKind::moveEffect && chain == edit.destination)
            {
                const auto& order = l.effectOrder[chain];
                const auto at = (int) (std::find (order.begin(), order.end(), edit.slot) - order.begin());
                const auto last = std::max (0, (int) edit.position - 1);
                edit.position = (std::uint8_t) (delta == 0 ? last : juce::jlimit (0, last, at + delta));
            }
        }
        unit.rackEdit (edit);
    }

    ProgrammerUnit& unit;
    const bool synth;
    const int index;
};

/** One page as the PROGRAMMER draws it, made once: its knobs in rows of engraved sections under their titles, with
    the tapes of the knobs racks name; or RACK LAYOUT's titles, rows and bank controls. */
class ProgrammerUnit::Leaf final : public juce::Component
{
public:
    /** A title, and the effect kind it names (-1 for SLOT and every section that is not an effect's kind). */
    struct Title { ShownTitle shown; juce::Path path; int kind = -1; };
    struct Tape { String text; juce::Image image; Pt centre; Rect body; float inkWidth = 0; };

    Leaf (ProgrammerUnit& u, const SubpageSpec& s) : spec (s), unit (u)
    {
        setInterceptsMouseClicks (false, true);
        setSize (u.getWidth(), u.getHeight());
        const auto& P = u.model.programmer;
        const auto rows = P["body"]["rows"];
        const auto left = num (P["body"]["x"][0]), right = num (P["body"]["x"][1]);
        if (s.rackLayout)
        {
            // Today's twelve bank rows, the synth banks left and the effect banks right; the selected bank's controls
            // in the second row's place under the synth banks.
            mid = std::round ((left + right) * 0.5f);
            strip = Rect::leftTopRightBottom (left, num (rows[1]["divider"][0]), mid - 2.0f, num (rows[1]["divider"][1]));
            lines.push_back ({ Rect::leftTopRightBottom (mid, num (rows[0]["divider"][0]), mid + 1.0f, num (rows[1]["divider"][1])), true });
            addTitles ({ "SYNTH BANKS", "EFFECT BANKS" }, { -1, -1 }, { mid }, rows[0]);
            const auto L = P["body"]["layout"];
            const auto baseline = num (rows[0]["title_baseline"]);
            for (int i = 0; i < 12; ++i)
            {
                const auto synth = i < 4;
                const auto n = synth ? i : i - 4;
                const auto x = synth ? left + num (L["title_dx_first"]) : mid + 2.0f + num (L["title_dx"]);
                const auto y = juce::roundToInt (baseline + 48.0f * (float) (n + 1));
                // The glass reaches title_dx_first left of the line, and the column's right.
                const auto x0 = juce::roundToInt (x - num (L["title_dx_first"]));
                auto& row = *bankRows.emplace_back (std::make_unique<BankRow> (u.look, x - (float) x0));
                row.setComponentID ((synth ? "synth." : "effect.") + String (n + 1) + ".row");
                row.setName ("Physical bank " + String (n + 1));
                row.setBounds (x0, y - (int) BankRow::baseline, juce::roundToInt (synth ? strip.getRight() : right) - x0, 28);
                row.setExplicitFocusOrder (i + 1);
                row.onClick = [this, i] { select (i); };
                addAndMakeVisible (row);
                auto& bank = *banks.emplace_back (std::make_unique<BankControls> (u, synth, n, strip));
                bank.setExplicitFocusOrder (13);
                addChildComponent (bank);
            }
            select (0);
            return;
        }
        for (std::size_t r = 0; r < s.rows.size() && r < 2; ++r)
        {
            const auto& sections = s.rows[r];
            const auto row = rows[(int) r];
            std::vector<int> counts, kinds;
            juce::StringArray names;
            for (const auto& section : sections)
            {
                counts.push_back ((int) section.knobs.size());
                names.add (section.title);
                kinds.push_back (s.effect >= 0 ? kindTitled (section.title) : -1);
            }
            const auto placed = layoutRow (P, counts, left, right);
            const auto y = num (row["knob_y"]);
            for (std::size_t si = 0; si < sections.size(); ++si)
                for (std::size_t ki = 0; ki < sections[si].knobs.size(); ++ki)
                    addKnob ((std::size_t) sections[si].knobs[ki].index, { placed.centres[si][ki], y });
            for (const auto d : placed.dividers)
                lines.push_back ({ Rect::leftTopRightBottom (d, num (row["divider"][0]), d + 1.0f, num (row["divider"][1])), true });
            addTitles (names, kinds, placed.dividers, row);
        }
        if (s.rows.size() > 1)
        {
            const auto sep = P["body"]["separator"];
            lines.push_back ({ Rect::leftTopRightBottom (num (sep["x"][0]), num (sep["y"]), num (sep["x"][1]), num (sep["y"]) + 1.0f), false });
        }
    }

    /** A name tape: the part's ends kept and its middle stretched to the width the name's ink needs by the skin's
        rule, the name in marker on its body, under the knob. */
    void letter (std::size_t k, const String& text)
    {
        auto& t = tapes[k];
        t = { text };
        if (text.isEmpty())
            return;
        const auto T = unit.model.programmer["tape"];
        const auto& a = unit.model.asset (T["part"].toString());
        const auto body = a.spec["body"];
        const auto bl = (float) (int) body[0], bt = (float) (int) body[1], br = (float) (int) body[2], bb = (float) (int) body[3];
        const auto sl = (int) T["slice"][0], sr = (int) T["slice"][2];
        const auto name = styledPath (unit.markerFaces, unit.tapeStyle, text, unit.tapeStyle.hscale);
        const auto ink = name.getBounds();
        const auto width = num (T["width"]["intercept"]) + num (T["width"]["slope"]) * ink.getWidth();
        const auto margin = (float) a.image.getWidth() - (br - bl);
        const auto outW = juce::roundToInt (width + margin), h = a.image.getHeight();
        t.image = juce::Image (juce::Image::ARGB, outW, h, true);
        juce::Graphics g (t.image);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (a.image, 0, 0, sl, h, 0, 0, sl, h);
        g.drawImage (a.image, sl, 0, outW - sl - sr, h, sl, 0, a.image.getWidth() - sl - sr, h);
        g.drawImage (a.image, outW - sr, 0, sr, h, a.image.getWidth() - sr, 0, sr, h);
        const auto st = unit.model.programmer["styles"]["tape"];
        t.centre = { bl + ((float) outW - margin) * 0.5f, (bt + bb) * 0.5f };
        g.setColour (fromBgr (st["bgr"]).withAlpha (num (st["alpha"])));
        g.fillPath (name, juce::AffineTransform::translation (t.centre.x - ink.getCentreX(), t.centre.y - ink.getCentreY()));
        t.inkWidth = ink.getWidth();
        const auto c = knobs[k]->centre();
        t.body = Rect (width, bb - bt).withCentre ({ c.x, c.y + num (T["dy"]) });
    }

    void paint (juce::Graphics& g) override
    {
        for (const auto& l : lines)
            unit.look.drawEngraving (g, l);
        for (const auto& l : stripLines)
            unit.look.drawEngraving (g, l);
        for (const auto& t : titles)
        {
            g.setColour (grey (t.shown.grey));
            g.fillPath (t.path);
        }
        const auto angle = juce::degreesToRadians (num (unit.model.programmer["tape"]["angle"]));
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        for (const auto& t : tapes)
            if (t.image.isValid())
                g.drawImageTransformed (t.image, juce::AffineTransform::translation (-t.centre.x, -t.centre.y)
                                                     .rotated (angle)
                                                     .translated (t.body.getCentreX(), t.body.getCentreY()));
    }

    const SubpageSpec& spec;
    /** Row by row; each knob's tape at its place in `tapes`. */
    std::vector<std::unique_ptr<ProgrammerKnob>> knobs;
    std::vector<Tape> tapes;
    std::vector<Title> titles;
    /** RACK LAYOUT's: the four synth banks, then the eight effect banks. */
    std::vector<std::unique_ptr<BankRow>> bankRows;
    std::vector<std::unique_ptr<BankControls>> banks;

private:
    /** Shows bank `i`'s controls under its name, its row on the glass. */
    void select (int i)
    {
        for (std::size_t k = 0; k < bankRows.size(); ++k)
        {
            bankRows[k]->setToggleState ((int) k == i, juce::dontSendNotification);
            banks[k]->setVisible ((int) k == i);
        }
        const auto& P = unit.model.programmer;
        const auto row = P["body"]["rows"][1];
        const auto synth = i < 4;
        const auto name = (synth ? "SYNTH " : "EFFECT ") + String ((synth ? i : i - 4) + 1);
        const auto laid = layoutTitles (P, unit.look.faces, { name }, {}, strip.getX(), mid - num (P["body"]["layout"]["rule_end"]),
                                        num (row["title_baseline"]), num (row["rule_y"]));
        titles.resize (2);
        titles.push_back ({ { name, laid.titles[0].ink, strip.getX(), unit.look.titleStyle.grey }, laid.titles[0].path, -1 });
        stripLines = laid.lines;
        repaint();
    }

    void addKnob (std::size_t index, Pt centre)
    {
        auto& k = *knobs.emplace_back (std::make_unique<ProgrammerKnob> (unit.processor, unit.look, unit.host.house, index));
        tapes.emplace_back();
        k.place (centre);
        k.setExplicitFocusOrder ((int) knobs.size());
        addAndMakeVisible (k);
        addAndMakeVisible (k.label);
        addChildComponent (k.entry);
        unit.byIndex[index] = &k;
    }

    void addTitles (const juce::StringArray& names, const std::vector<int>& kinds, const std::vector<float>& dividers, const juce::var& row)
    {
        const auto& P = unit.model.programmer;
        const auto left = num (P["body"]["x"][0]);
        const auto laid = layoutTitles (P, unit.look.faces, names, dividers, left, num (P["body"]["x"][1]) + 1.0f, num (row["title_baseline"]),
                                        num (row["rule_y"]));
        for (std::size_t i = 0; i < laid.titles.size(); ++i)
        {
            const auto& t = laid.titles[i];
            titles.push_back ({ { t.text, t.ink, i == 0 ? left : dividers[i - 1] + 2.0f, unit.look.titleStyle.grey }, t.path, kinds[i] });
        }
        lines.insert (lines.end(), laid.lines.begin(), laid.lines.end());
    }

    ProgrammerUnit& unit;
    std::vector<Engraving> lines, stripLines;
    /** RACK LAYOUT's divider, and where the selected bank's controls go. */
    float mid = 0;
    Rect strip;
};

ProgrammerUnit::ProgrammerUnit (InstrumentProcessor& p, const Model& m, ProgrammerHost h)
    : processor (p), model (m), look (m), marker (m.markerFace), markerFaces { marker, marker }, tapeStyle (styleOf (m.programmer, "tape")),
      host (std::move (h)), ctx { m, look.mono, look.caps }
{
    const auto& plate = model.plates.at ("advanced");
    setSize (plate.getWidth(), plate.getHeight());
    setComponentID ("pages");
    setTitle ("Programmer");
    setFocusContainerType (FocusContainerType::focusContainer);
    setLookAndFeel (&look);

    const auto addLeaf = [this] (const SubpageSpec& s)
    {
        auto& leaf = *leaves.emplace_back (std::make_unique<Leaf> (*this, s));
        leaf.setExplicitFocusOrder (30);
        addChildComponent (leaf);
        if (s.rackLayout)
            layoutLeaf = &leaf;
    };
    std::size_t most = 0;
    for (const auto& page : model.pages)
    {
        most = std::max (most, page.subpages.size());
        for (const auto& sub : page.subpages)
        {
            if (sub.routes.empty())
                addLeaf (sub);
            for (const auto& r : sub.routes)
                addLeaf (r);
        }
    }
    for (auto& leaf : leaves)
        for (auto& k : leaf->knobs)
        {
            const auto i = k->index;
            k->onTouched = [this] (ProgrammerKnob& touched)
            {
                lastTouched = &touched;
                showDisplay();
            };
            if (i >= 1 && i <= 8)
                k->addToMenu = [this, n = (int) i] (ProgrammerKnob::Menu& menu) { macroMenu (n, menu); };
            if (i >= 252 && i < 572 && (i - 252) % 5 != 4)
                k->onEdit = [this, i] (float physical) { route (i, processor.control (i).convertTo0to1 (physical)); };
            if (parameterRegistry()[i].id == "output.protection")
                k->setTooltip ("Stereo-linked sample-peak protection at -1 dBFS. Off preserves unrestricted output.");
        }

    const auto& lit = model.asset (model.programmer["lit"].toString());
    for (const auto& page : model.pages)
    {
        auto& e = pageElements.emplace_back();
        e.id = "advanced.page." + page.id;
        e.view = "advanced";
        e.kind = "page";
        e.label = page.label;
        e.control = "programmer.page";
        e.rect = page.rect;
        e.led = page.led;
        e.lensR = page.lensR;
        e.layers["lit"] = model.programmer["lit"].toString();
        auto& b = *pageButtons.emplace_back (std::make_unique<SkinButton> (ctx, e, lit.lensR > 0.0f ? page.lensR / lit.lensR : 1.0f));
        b.setComponentID ("programmer.page." + page.id);
        b.setExplicitFocusOrder ((int) pageButtons.size());
        b.onClick = [this, id = page.id] { showPage (id); };
        addAndMakeVisible (b);
    }
    for (std::size_t i = 0; i < most; ++i)
    {
        auto& b = *subpageButtons.emplace_back (std::make_unique<KeyFocus<juce::ToggleButton>>());
        b.setClickingTogglesState (false);
        b.setToggleable (true);
        b.setExplicitFocusOrder (10 + (int) i);
        b.onClick = [this, i]
        {
            const auto& subpages = model.page (pageId).subpages;
            if (i < subpages.size())
                showSubpage (subpages[i].id);
        };
        addChildComponent (b);
    }

    displayElement.id = "advanced.display";
    displayElement.view = "advanced";
    displayElement.kind = "display";
    displayElement.label = "Programmer display";
    displayElement.glass = model.displays.at ("advanced").lattice.glass;
    display = std::make_unique<Display> (ctx, displayElement);
    display->setComponentID ("programmer.display");
    display->setExplicitFocusOrder (20);
    addAndMakeVisible (*display);

    assign.setComponentID ("programmer.assign");
    assign.target.setComponentID ("programmer.assign.target");
    assign.depth.setComponentID ("programmer.assign.depth");
    assign.clear.setComponentID ("programmer.assign.clear");
    assign.done.setComponentID ("programmer.assign.close");
    assign.setExplicitFocusOrder (40);
    assign.onAssign = [this] (int n, const house::Assignment& a) { assignMacro (n, a); };
    assign.onPreview = [this] (int n, std::optional<house::Assignment> a)
    {
        if (a)
            preview.emplace (n, *a);
        else
            preview.reset();
        letterTapes();
    };
    addChildComponent (assign);

    for (std::size_t n = 0; n < effectStatus.size(); ++n)
    {
        effectStatus[n].setComponentID ("effect." + String ((int) n + 1) + ".status");
        effectStatus[n].setTitle ("Active physical effect and owner");
        addChildComponent (effectStatus[n]);
    }

    refresh (shown);
    show();
}

ProgrammerUnit::~ProgrammerUnit() { for(auto* knob:byIndex)if(knob)knob->closeEntry();setLookAndFeel (nullptr); }

void ProgrammerUnit::refresh (const SoundDocument& d, bool recalled)
{
    std::array<bool, 8> effectChanged {};
    for (std::size_t n = 0; n < effectChanged.size(); ++n)
        effectChanged[n] = ! refreshed || recalled || d.layout.effects[n] != shown.layout.effects[n];
    for (std::size_t i = 0; i < parameterCount; ++i)
        if (auto* knob=byIndex[i])
        {
            if (! refreshed || d.controlLocks[i] != shown.controlLocks[i])knob->setLocked(d.controlLocks[i]);
            showDynamicEqRelevance(*knob,d.values);
            showSaturationRelevance(*knob,d);
            if(!showSpaceReverbRelevance(*knob,d.values))return;
        }
    auto letter = ! refreshed || d.macros != shown.macros;
    const auto layoutChanged = ! refreshed || d.layout != shown.layout;
    if (! refreshed || d.personalities != shown.personalities || layoutChanged || d.values != shown.values)
    {
        if (auto now = rackTargets (d); now != targets)
        {
            targets = std::move (now);
            assign.setRackTargets (targets);
            letter = true;
        }
        // Each bank's owner: the first placed rack with a unit there.
        synthOwners = {};
        effectOwners = {};
        if (d.personalities)
            for (const auto& i : d.personalities->instances)
                for (const auto& b : i.bindings)
                    if (const auto* u = unitOf (i, b.unit))
                    {
                        const auto synth = isSourceModule (u->module);
                        if (synth && b.slot < synthOwners.size() && synthOwners[b.slot].isEmpty())
                            synthOwners[b.slot] = i.design.name.toUpperCase();
                        else if (! synth && b.slot < effectOwners.size() && effectOwners[b.slot].isEmpty())
                            effectOwners[b.slot] = i.design.name.toUpperCase();
                    }
    }
    if (&d != &shown)
        shown = d;
    refreshed = true;
    if (letter)
        letterTapes();
    if (layoutChanged)
        showLayout();
    if (layoutLeaf != nullptr)
        for (std::size_t i = 0; i < layoutLeaf->banks.size(); ++i)
            layoutLeaf->banks[i]->refresh (shown, i >= 4 && effectChanged[i - 4]);
    if (assign.macro != 0 && ! assign.isDragging())
        if (const auto a = assignmentOf (d.macros[(std::size_t) assign.macro - 1]); ! same (assign.assignment(), a))
            assign.show (a);
    showDisplay();
}

void ProgrammerUnit::showPage (const String& id)
{
    if (id != pageId)
        assign.close (false);
    lastSubpage[pageId] = subpageId;
    pageId = id;
    const auto it = lastSubpage.find (id);
    subpageId = it != lastSubpage.end() ? it->second : model.page (id).subpages.front().id;
    openEffect();
    show();
}

void ProgrammerUnit::showSubpage (const String& id)
{
    if (id != subpageId)
    {
        subpageId = id;
        openEffect();
    }
    else
        for (const auto& s : model.page (pageId).subpages)
            if (s.id == id && ! s.routes.empty())
            {
                auto& at=s.id=="drum"?drumPages[std::size_t(model.page(pageId).synth)]:s.effect>=0?fxPage:routes;
                at = (at + 1) % (int) s.routes.size();
            }
    show();
}

const SubpageSpec& ProgrammerUnit::shownSpec() const
{
    const auto& p=model.page(pageId);
    if(subpageId=="drum"&&p.synth>=0&&p.synth<4)
        return model.leaf(pageId,subpageId,drumPages[std::size_t(p.synth)]);
    for (const auto& s : model.page (pageId).subpages)
        if (s.id == subpageId && s.effect >= 0)
            return model.leaf (pageId, subpageId, fxPage);
    return model.leaf (pageId, subpageId, routes);
}

void ProgrammerUnit::openEffect()
{
    for (const auto& s : model.page (pageId).subpages)
        if (s.id == subpageId && s.effect >= 0)
            fxPage = pageOfKind (s, shown.layout.effects[(std::size_t) s.effect].kind);
}

ProgrammerKnob* ProgrammerUnit::knob (std::size_t index) const { return index < byIndex.size() ? byIndex[index] : nullptr; }

std::vector<ProgrammerKnob*> ProgrammerUnit::shownKnobs() const
{
    std::vector<ProgrammerKnob*> out;
    if (shownLeaf != nullptr)
        for (auto& k : shownLeaf->knobs)
            out.push_back (k.get());
    return out;
}

std::vector<ProgrammerUnit::ShownTitle> ProgrammerUnit::shownTitles() const
{
    std::vector<ShownTitle> out;
    if (shownLeaf != nullptr)
        for (auto& t : shownLeaf->titles)
            out.push_back (t.shown);
    return out;
}

std::vector<ProgrammerUnit::ShownTape> ProgrammerUnit::shownTapes() const
{
    std::vector<ShownTape> out;
    if (shownLeaf != nullptr)
        for (std::size_t k = 0; k < shownLeaf->knobs.size(); ++k)
            if (const auto& t = shownLeaf->tapes[k]; t.text.isNotEmpty())
                out.push_back ({ shownLeaf->knobs[k]->index, t.text, t.body, t.inkWidth });
    return out;
}

std::vector<juce::Button*> ProgrammerUnit::rackRows() const
{
    std::vector<juce::Button*> out;
    if (layoutLeaf != nullptr)
        for (auto& r : layoutLeaf->bankRows)
            out.push_back (r.get());
    return out;
}

String ProgrammerUnit::bankLine() const
{
    const auto& p = model.page (pageId);
    const auto& l = shown.layout;
    if (p.id == "global")
        return "GLOBAL  SHARED BY EVERY RACK";
    if (p.id == "layout")
    {
        const auto synths = (int) std::count (l.synthUse.begin(), l.synthUse.end(), SlotUse::present);
        const auto effects = (int) std::count_if (l.effects.begin(), l.effects.end(), [] (const EffectSlot& e) { return e.use == SlotUse::present; });
        return "RACK LAYOUT  " + String (synths) + (synths == 1 ? " SYNTH  " : " SYNTHS  ") + String (effects) + (effects == 1 ? " EFFECT" : " EFFECTS");
    }
    const auto owned = [] (const String& owner) { return owner.isEmpty() ? String() : "  OWNED BY " + owner; };
    if (p.synth >= 0)
    {
        const auto n = (std::size_t) p.synth;
        const auto* sub = &p.subpages.front();
        for (const auto& s : p.subpages)
            if (s.id == subpageId)
                sub = &s;
        auto label = sub->label;
        if (! sub->routes.empty())
        {
            const auto at=sub->id=="drum"?drumPages[n]:routes;
            const auto& r = sub->routes[(std::size_t) juce::jlimit (0, (int) sub->routes.size() - 1, at)];
            label << " " << r.first << "-" << r.last;
        }
        const auto head = "SYNTH " + String ((int) n + 1) + "  " + label;
        if (l.synthUse[n] != SlotUse::present)
            return head + (l.synthUse[n] == SlotUse::retired ? "  RETIRED" : "  UNUSED");
        const auto wavetable = shown.values[13 + 4 * n] >= 0.5f, on = shown.values[12 + 4 * n] >= 0.5f;
        const bool drum = shown.values[drumModeParameterStart + n] >= 0.5f;
        const auto state = sub->id == "drum" ? (drum ? "ACTIVE" : "RETAINED")
                           : drum && sub->id != "common" ? "RETAINED"
                           : sub->engine == 0 ? (drum || wavetable ? "RETAINED" : "ACTIVE")
                           : sub->engine == 1 ? (!drum && wavetable ? "ACTIVE" : "RETAINED")
                                              : (on ? "ON" : "OFF");
        return head + "  " + state + owned (synthOwners[n]);
    }
    const auto n = (std::size_t) std::max (0, shownSpec().effect);
    const auto& slot = l.effects[n];
    const auto head = "FX " + String ((int) n + 1) + "  " + effectDisplayName (slot.kind).toUpperCase();
    if (slot.use != SlotUse::present)
        return head + (slot.use == SlotUse::retired ? "  RETIRED" : "  INACTIVE BANK");
    return head + "  " + (slot.chain == masterChain ? String ("MASTER") : "SYNTH " + String (slot.chain + 1) + " INSERT") + owned (effectOwners[n]);
}

void ProgrammerUnit::paint (juce::Graphics& g) { g.drawImageAt (model.plates.at ("advanced"), 0, 0); }

void ProgrammerUnit::visibilityChanged()
{
    if (! isVisible())
    {
        const juce::Component::SafePointer<ProgrammerUnit> alive(this);
        for(auto* knob:byIndex){if(knob)knob->closeEntry();if(!alive)return;}
        assign.close (false);
    }
}

void ProgrammerUnit::show()
{
    if (auto* leaf = leafFor (shownSpec()); leaf != shownLeaf)
    {
        if (shownLeaf != nullptr)
        {
            const juce::Component::SafePointer<ProgrammerUnit> alive(this);
            for(auto& knob:shownLeaf->knobs){knob->closeEntry();if(!alive)return;}
            shownLeaf->setVisible (false);
        }
        shownLeaf = leaf;
        if (shownLeaf != nullptr)
            shownLeaf->setVisible (true);
    }
    for (std::size_t i = 0; i < pageButtons.size(); ++i)
        pageButtons[i]->setToggleState (model.pages[i].id == pageId, juce::dontSendNotification);

    // A page with one unnamed sub-page has no sub-page buttons; the buttons squeeze their pitch to fit before last_max.
    const auto& p = model.page (pageId);
    const auto n = p.subpages.size() > 1 || p.subpages.front().label.isNotEmpty() ? p.subpages.size() : std::size_t { 0 };
    const auto sp = model.programmer["subpages"];
    const auto first = num (sp["first"]), last = num (sp["last_max"]);
    auto pitch = num (sp["pitch"]);
    if (n > 1 && first + (float) (n - 1) * pitch > last)
        pitch = (last - first) / (float) (n - 1);
    const auto w = std::min (84, (int) std::floor (pitch));
    // The look draws a button's face 30 px down it, under its lit LED's glow.
    const auto top = juce::roundToInt (num (sp["top"])) - 30;
    for (std::size_t i = 0; i < subpageButtons.size(); ++i)
    {
        auto& b = *subpageButtons[i];
        if (i >= n)
        {
            b.setVisible (false);
            continue;
        }
        const auto& s = p.subpages[i];
        b.setButtonText (s.label);
        b.setTitle (s.label);
        b.setComponentID ("programmer.subpage." + s.id);
        b.setToggleState (s.id == subpageId, juce::dontSendNotification);
        b.setBounds (juce::roundToInt (first + (float) i * pitch - (float) w * 0.5f), top, w, 96);
        b.setVisible (true);
    }
    showDisplay();
    repaint();
}

void ProgrammerUnit::showDisplay()
{
    if (display == nullptr)
        return;
    ProgrammerShown s;
    s.bank = bankLine();
    s.value = lastTouched != nullptr ? lastTouched->valueLine() : String();
    const auto effect = shownSpec().effect;
    const auto said = effect >= 0 ? effectStatus[(std::size_t) effect].getText() : s.bank;
    s.accessible = s.value.isEmpty() ? said : said + (said.endsWithChar ('.') ? " " : ". ") + s.value;
    display->show (s);
}

void ProgrammerUnit::showLayout()
{
    const auto& l = shown.layout;
    for (std::size_t n = 0; n < effectStatus.size(); ++n)
    {
        const auto& e = l.effects[n];
        const auto owner = e.chain == masterChain ? String ("Master") : "Synth " + String (e.chain + 1) + " insert";
        effectStatus[n].setText ("Physical effect " + String ((int) n + 1) + " / " + effectDisplayName (e.kind) + " / " + owner + " / "
                                     + (e.use == SlotUse::present ? "Present" : "Inactive bank") + ". Change type or owner in Rack layout.",
                                 juce::dontSendNotification);
    }
    const auto retained = (int) model.programmer["styles"]["title"]["retained_grey"];
    for (auto& leaf : leaves)
    {
        if (leaf->spec.effect < 0)
            continue;
        const auto& slot = l.effects[(std::size_t) leaf->spec.effect];
        for (auto& t : leaf->titles)
            t.shown.grey = t.kind >= 0 && (slot.use != SlotUse::present || (int) slot.kind != t.kind) ? retained : look.titleStyle.grey;
        leaf->repaint();
    }
    if (layoutLeaf != nullptr)
        for (std::size_t i = 0; i < layoutLeaf->bankRows.size(); ++i)
        {
            const auto synth = i < 4;
            auto& row = *layoutLeaf->bankRows[i];
            row.setButtonText (rowText (shown, synth, synth ? i : i - 4));
            row.setDescription (rowSentence (shown, synth, synth ? i : i - 4));
        }
}

void ProgrammerUnit::letterTapes()
{
    for (auto& leaf : leaves)
    {
        auto changed = false;
        for (std::size_t k = 0; k < leaf->knobs.size(); ++k)
            if (const auto text = tapeText (leaf->knobs[k]->index); text != leaf->tapes[k].text)
            {
                leaf->letter (k, text);
                changed = true;
            }
        if (changed)
            leaf->repaint();
    }
}

String ProgrammerUnit::tapeText (std::size_t index) const
{
    if (index >= 1 && index <= 8)
    {
        const auto a = preview && preview->first == (int) index ? preview->second : assignmentOf (shown.macros[index - 1]);
        if (! a.target)
            return {};
        const auto pc = juce::roundToInt (a.amount * 100.0f);
        return targetName (*a.target, targets, model) + " " + (pc < 0 ? "-" : "+") + String (std::abs (pc));
    }
    for (const auto& t : targets)
        if (t.index == index)
            return t.label;
    return {};
}

void ProgrammerUnit::route (std::size_t index, float value)
{
    // A rejected or unattempted edit shows every route field as the processor holds it.
    editRoute (processor, host, index, value, [this]
               {
                   for (std::size_t i = 252; i < 572; ++i)
                       if (auto* k = byIndex[i])
                           k->reread();
               });
}

void ProgrammerUnit::rackEdit (const RackEdit& edit)
{
    const auto scope = frame();
    if (closing())
        return;
    SoundDocument d;
    std::uint64_t revision = 0;
    if (! host.snapshot || ! host.snapshot (d, revision))
        return;
    const auto outcome = processor.applyRackEdit (edit, revision);
    if (closing())
        return;
    if (host.house.report)
        host.house.report (outcome == RackEditResult::replacementRequired
                               ? "Physical bank " + String (edit.slot + 1) + " retains prior intent. Use its explicit Replace button."
                           : outcome == RackEditResult::capacityReached ? String ("Rack capacity reached")
                                                                        : String ("Rack edit") + (outcome == RackEditResult::applied ? " complete" : " failed; sound preserved"));
    if (host.refresh)
        host.refresh();
}

void ProgrammerUnit::macroMenu (int macro, ProgrammerKnob::Menu& m)
{
    const auto slot = (std::size_t) macro - 1;
    if (lastTouched != nullptr && macroTarget (lastTouched->index))
        m.push_back ({ "Assign to " + parameterRegistry()[lastTouched->index].name, [this, macro, slot, target = lastTouched->index]
                       {
                           const auto a = assignmentOf (shown.macros[slot]);
                           assignMacro (macro, { target, a.target ? a.amount : 0.5f });
                       } });
    if (assignmentOf (shown.macros[slot]).target)
    {
        for (const auto step : { 0.1f, -0.1f })
            m.push_back ({ step > 0.0f ? "Depth +10%" : "Depth -10%", [this, macro, slot, step]
                           {
                               if (const auto a = assignmentOf (shown.macros[slot]); a.target)
                                   assignMacro (macro, { a.target, juce::jlimit (-1.0f, 1.0f, a.amount + step) });
                           } });
        m.push_back ({ "Clear", [this, macro] { assignMacro (macro, {}); } });
    }
    m.push_back ({ "Edit assignment", [this, macro] { editAssignment (macro); } });
}

void ProgrammerUnit::assignMacro (int macro, const house::Assignment& a)
{
    const auto scope = frame();
    if (closing())
        return;
    const auto slot = (std::size_t) macro - 1;
    const auto binding = a.target ? MacroBinding { parameterRegistry()[*a.target].stateKey, a.amount } : MacroBinding {};
    if (! host.house.change || binding == shown.macros[slot])
        return;
    host.house.change ([slot, binding] (SoundDocument& d) { d.macros[slot] = binding; }, a.target ? "Macro assignment" : "Clear macro");
}

void ProgrammerUnit::editAssignment (int macro)
{
    auto* k = knob ((std::size_t) macro);
    if (k == nullptr)
        return;
    assign.open (macro, *k, k->getBounds().toFloat().getUnion (k->label.getBounds().toFloat()));
    assign.show (assignmentOf (shown.macros[(std::size_t) macro - 1]));
}

ProgrammerUnit::Leaf* ProgrammerUnit::leafFor (const SubpageSpec& s) const
{
    for (auto& leaf : leaves)
        if (&leaf->spec == &s)
            return leaf.get();
    return nullptr;
}

std::shared_ptr<void> ProgrammerUnit::frame() const { return host.house.frame ? host.house.frame() : nullptr; }

bool ProgrammerUnit::closing() const { return host.house.closing && host.house.closing(); }
} // namespace airack::studio
