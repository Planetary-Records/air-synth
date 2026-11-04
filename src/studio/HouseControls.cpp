#include "HouseControls.h"
#include "ParameterListenerRetirement.h"
#include "SliderAccessibility.h"

#include "InstrumentProcessor.h"
#include "ParameterRegistry.h"
#include "WheelLatch.h"

#include <algorithm>
#include <cmath>
#include <utility>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <gdiplus.h>
#endif

namespace airack::studio::house
{
GeneratedText::GeneratedText()
{
   #if JUCE_WINDOWS
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR started = 0;
    if (Gdiplus::GdiplusStartup (&started, &input, nullptr) == Gdiplus::Ok)
        token = started;
   #endif
}

GeneratedText::~GeneratedText()
{
   #if JUCE_WINDOWS
    if (token != 0)
        Gdiplus::GdiplusShutdown (token);
   #endif
}

bool GeneratedText::draw (juce::Graphics& g, const String& text, const juce::Font& font, juce::Colour ink,
                          juce::Rectangle<float> area, bool centred) const
{
   #if JUCE_WINDOWS
    if (! available()) return false;
    const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (! std::isfinite (scale) || scale <= 0) return false;
    const auto visible = area.expanded (2.0f).getIntersection (g.getClipBounds().toFloat());
    if (visible.isEmpty()) return true;
    const auto pixels = (visible * scale).getSmallestIntegerContainer();
    try
    {
        juce::Image image (juce::Image::ARGB, pixels.getWidth(), pixels.getHeight(), true, juce::SoftwareImageType());
        {
            juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
            Gdiplus::Bitmap bitmap (image.getWidth(), image.getHeight(), data.lineStride, PixelFormat32bppPARGB, data.data);
            Gdiplus::Graphics glyphs (&bitmap);
            Gdiplus::FontFamily family (font.getTypefaceName().toWideCharPointer());
            const auto style = font.isBold() ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
            if (bitmap.GetLastStatus() != Gdiplus::Ok || glyphs.GetLastStatus() != Gdiplus::Ok
                || family.GetLastStatus() != Gdiplus::Ok || ! family.IsStyleAvailable (style)) return false;
            Gdiplus::Font nativeFont (&family, font.getHeightInPoints(), style, Gdiplus::UnitPixel);
            if (nativeFont.GetLastStatus() != Gdiplus::Ok) return false;
            glyphs.SetInterpolationMode (Gdiplus::InterpolationModeHighQualityBicubic);
            glyphs.SetPixelOffsetMode (Gdiplus::PixelOffsetModeHighQuality);
            glyphs.SetTextRenderingHint (Gdiplus::TextRenderingHintAntiAliasGridFit);
            Gdiplus::Matrix transform (scale, 0, 0, scale, -float (pixels.getX()), -float (pixels.getY()));
            glyphs.SetTransform (&transform);
            Gdiplus::SolidBrush brush (Gdiplus::Color (ink.getARGB()));
            Gdiplus::Status result;
            if (centred)
            {
                Gdiplus::StringFormat format;
                format.SetAlignment (Gdiplus::StringAlignmentCenter);
                format.SetLineAlignment (Gdiplus::StringAlignmentCenter);
                result = glyphs.DrawString (text.toWideCharPointer(), -1, &nativeFont,
                    Gdiplus::RectF (area.getX(), area.getY(), area.getWidth(), area.getHeight()), &format, &brush);
            }
            else
                result = glyphs.DrawString (text.toWideCharPointer(), -1, &nativeFont,
                    Gdiplus::PointF (area.getX(), area.getY()), Gdiplus::StringFormat::GenericTypographic(), &brush);
            if (result != Gdiplus::Ok) return false;
        }
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale)
                                       .translated (float (pixels.getX()) / scale, float (pixels.getY()) / scale));
        return true;
    }
    catch (const std::bad_alloc&) { return false; }
   #else
    juce::ignoreUnused (g, text, font, ink, area, centred);
    return false;
   #endif
}
bool GeneratedText::drawSprite (juce::Graphics& g, const juce::Image& source,
                                Pt anchor, Pt pivot, Pt scale) const
{
   #if JUCE_WINDOWS
    if (! available() || ! source.isValid() || source.getFormat() != juce::Image::ARGB
        || ! std::isfinite (anchor.x) || ! std::isfinite (anchor.y)
        || ! std::isfinite (pivot.x) || ! std::isfinite (pivot.y)
        || ! std::isfinite (scale.x) || ! std::isfinite (scale.y) || scale.x <= 0 || scale.y <= 0)
        return false;
    const auto physicalScale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (! std::isfinite (physicalScale) || physicalScale <= 0) return false;
    const juce::Rectangle<float> destination (pivot.x - anchor.x * scale.x, pivot.y - anchor.y * scale.y,
                                               source.getWidth() * scale.x, source.getHeight() * scale.y);
    if (! std::isfinite (destination.getX()) || ! std::isfinite (destination.getY())
        || ! std::isfinite (destination.getRight()) || ! std::isfinite (destination.getBottom())) return false;
    const auto visible = destination.expanded (2.0f).getIntersection (g.getClipBounds().toFloat());
    if (visible.isEmpty()) return true;
    const auto pixels = (visible * physicalScale).getSmallestIntegerContainer();
    try
    {
        juce::Image carrier (juce::Image::ARGB, pixels.getWidth(), pixels.getHeight(), true, juce::SoftwareImageType());
        if (! carrier.isValid()) return false;
        {
            juce::Image::BitmapData input (source, juce::Image::BitmapData::readOnly);
            juce::Image::BitmapData output (carrier, juce::Image::BitmapData::writeOnly);
            if (input.pixelStride != 4 || input.lineStride <= 0 || output.pixelStride != 4 || output.lineStride <= 0)
                return false;
            Gdiplus::Bitmap image (source.getWidth(), source.getHeight(), input.lineStride, PixelFormat32bppPARGB, input.data);
            Gdiplus::Bitmap bitmap (carrier.getWidth(), carrier.getHeight(), output.lineStride, PixelFormat32bppPARGB, output.data);
            Gdiplus::Graphics sprites (&bitmap);
            Gdiplus::Matrix transform (physicalScale, 0, 0, physicalScale,
                                       physicalScale * pivot.x - float (pixels.getX()),
                                       physicalScale * pivot.y - float (pixels.getY()));
            if (image.GetLastStatus() != Gdiplus::Ok || bitmap.GetLastStatus() != Gdiplus::Ok
                || sprites.GetLastStatus() != Gdiplus::Ok || transform.GetLastStatus() != Gdiplus::Ok
                || sprites.SetInterpolationMode (Gdiplus::InterpolationModeHighQualityBicubic) != Gdiplus::Ok
                || sprites.SetPixelOffsetMode (Gdiplus::PixelOffsetModeHighQuality) != Gdiplus::Ok
                || sprites.SetTransform (&transform) != Gdiplus::Ok) return false;
            if (sprites.DrawImage (&image,
                    Gdiplus::RectF (-anchor.x * scale.x, -anchor.y * scale.y,
                                   source.getWidth() * scale.x, source.getHeight() * scale.y),
                    0, 0, float (source.getWidth()), float (source.getHeight()), Gdiplus::UnitPixel) != Gdiplus::Ok)
                return false;
        }
        g.drawImageTransformed (carrier, juce::AffineTransform::scale (1.0f / physicalScale)
                                         .translated (float (pixels.getX()) / physicalScale, float (pixels.getY()) / physicalScale));
        return true;
    }
    catch (const std::bad_alloc&) { return false; }
   #else
    juce::ignoreUnused (g, source, anchor, pivot, scale);
    return false;
   #endif
}
//------------------------------------------------------------------------------ Readout

void Readout::show (const String& t, juce::Rectangle<float> beside, juce::Rectangle<int> limits)
{
    text = t;
    const auto w = 30 + juce::roundToInt (juce::GlyphArrangement::getStringWidth (font(), text));
    const auto y = juce::roundToInt (beside.getCentreY()) - 20;
    auto r = juce::Rectangle<int> (juce::roundToInt (beside.getRight()) + 10, y, w, 40);
    if (r.getRight() > limits.getRight())
        r.setX (juce::roundToInt (beside.getX()) - 10 - w);
    setBounds (r.constrainedWithin (limits));
    setVisible (true);
    repaint();
}

void Readout::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (juce::Colour (0xf0101113));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (accent);
    g.drawRoundedRectangle (r, 6.0f, 1.5f);
    g.setColour (juce::Colour (0xfff2f2ee));
    g.setFont (font());
    g.drawText (text, getLocalBounds(), juce::Justification::centred);
}

//------------------------------------------------------------------------------ BoundSlider, Knob, Fader

BoundSlider::BoundSlider (View& ownerIn, const Element& el, int dragPixels)
    : juce::Slider (juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox), element (el),
      param (ownerIn.ctx.processor.control (el.paramIndex)), owner (ownerIn),
      gesture (ownerIn.ctx.processor, el.paramIndex, ownerIn.ctx.host, [this] (float physical) { read (physical); })
{
    if (el.logarithmic)
        setNormalisableRange (juce::NormalisableRange<double> (el.range.getStart(), el.range.getEnd(),
                                                               [] (double lo, double hi, double p) { return lo * std::pow (hi / lo, p); },
                                                               [] (double lo, double hi, double v) { return std::log (v / lo) / std::log (hi / lo); }));
    else if (el.registryResponse && el.skew != 1.0)
        setNormalisableRange (juce::NormalisableRange<double> (el.range.getStart(), el.range.getEnd(), 0.0, el.skew));
    else
        setRange (el.range.getStart(), el.range.getEnd(), el.detents ? 1.0 : 0.0);
    setMouseDragSensitivity (dragPixels);
    setVelocityModeParameters (0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    setDoubleClickReturnValue (true, el.defaultValue);
    setWantsKeyboardFocus (true);
    setTitle (el.label);

    attachment = std::make_unique<juce::ParameterAttachment> (param, [this] (float physical) { read (physical); });
    onDragStart = [this]
    {
        const auto frame = owner.ctx.frame();
        const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
        resumeInput();
        gesture.begin();
        owner.ctx.touch (element);
    };
    onDragEnd = [this]
    {
        const auto frame = owner.ctx.frame();
        const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
        const auto wasHeld = gesture.held();
        gesture.end();
        if (! isMouseOver (true) && ! showsFocusRing())
            owner.hideReadout (*this);
        const auto ended = wasHeld ? owner.onGestureEnd : std::function<void()> {};
        if (ended)
            ended();
    };
    onValueChange = [this]
    {
        const auto frame = owner.ctx.frame();
        const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
        if (suspended)
        {
            setValue (param.convertFrom0to1 (param.getValue()), juce::dontSendNotification);
            return;
        }
        gesture.write ((float) getValue());
        owner.ctx.touch (element);
        shown();
    };
    attachment->sendInitialUpdate();
}

BoundSlider::~BoundSlider()
{
    gesture.end();
    // juce::Slider ends a held drag as it goes, after this part of it has gone.
    onDragStart = onDragEnd = onValueChange = nullptr;
    attachment.reset();
}

void BoundSlider::retire()
{
    gesture.retire();
    onDragStart = onDragEnd = onValueChange = nullptr;
    detail::ParameterListenerDispatch::retire(owner.ctx.processor,attachment);setEnabled(false);
}

std::unique_ptr<juce::AccessibilityHandler> BoundSlider::createAccessibilityHandler()
{
    return std::make_unique<detail::SliderAccessibility> (*this, owner.ctx.processor, [this] { return owner.ctx.frame(); });
}

void BoundSlider::shown()
{
    owner.elementChanged (*this);
    repaint();
}

void BoundSlider::read (float physical)
{
    setValue (physical, juce::dontSendNotification);
    shown();
}

bool BoundSlider::keyPressed (const juce::KeyPress& key)
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    resumeInput();
    const auto code = key.getKeyCode();
    if (element.detents)
    {
        // A detented knob steps one position a press.
        auto v = getValue();
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey || code == juce::KeyPress::pageUpKey)            v += 1.0;
        else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey || code == juce::KeyPress::pageDownKey)  v -= 1.0;
        else if (code == juce::KeyPress::homeKey)  v = getMinimum();
        else if (code == juce::KeyPress::endKey)   v = getMaximum();
        else return false;
        setValue (juce::jlimit (getMinimum(), getMaximum(), v), juce::sendNotificationSync);
        owner.showReadout (*this);
        return true;
    }
    const auto fine = key.getModifiers().isShiftDown();
    auto step = 0.0;
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)          step = fine ? 0.001 : 0.01;
    else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)    step = fine ? -0.001 : -0.01;
    else if (code == juce::KeyPress::pageUpKey)    step = 0.1;
    else if (code == juce::KeyPress::pageDownKey)  step = -0.1;
    else if (code == juce::KeyPress::homeKey)      step = -1.0;
    else if (code == juce::KeyPress::endKey)       step = 1.0;
    else return false;

    setValue (proportionOfLengthToValue (juce::jlimit (0.0, 1.0, proportion() + step)), juce::sendNotificationSync);
    owner.showReadout (*this);
    return true;
}

// JUCE continues using its slider after synchronous gesture/value callbacks return.
void BoundSlider::mouseDown (const juce::MouseEvent& e)
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    resumeInput();
    juce::Slider::mouseDown (e);
}

void BoundSlider::mouseDrag (const juce::MouseEvent& e)
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    juce::Slider::mouseDrag (e);
}

void BoundSlider::mouseUp (const juce::MouseEvent& e)
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    juce::Slider::mouseUp (e);
}

void BoundSlider::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    resumeInput();
    juce::Slider::mouseDoubleClick (e);
}

void BoundSlider::mouseEnter (const juce::MouseEvent& e)
{
    juce::Slider::mouseEnter (e);
    owner.showReadout (*this);
}

void BoundSlider::mouseExit (const juce::MouseEvent& e)
{
    juce::Slider::mouseExit (e);
    if (! gesture.held() && ! showsFocusRing())
        owner.hideReadout (*this);
}

void BoundSlider::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    if (rackScrolling())
        juce::Component::mouseWheelMove (e, w);
    else
    {
        resumeInput();
        juce::Slider::mouseWheelMove (e, w);
    }
}

void BoundSlider::suspend()
{
    const auto scope = owner.ctx.frame();
    const auto wasHeld = gesture.held();
    suspended = true;
    gesture.retire();
    const auto ended = wasHeld ? owner.onGestureEnd : std::function<void()> {};
    if (ended)
        ended();
}

void BoundSlider::focusGained (FocusChangeType cause)
{
    owner.ctx.touch (element);
    focusFromKeyboard = cause == focusChangedByTabKey;
    if (focusFromKeyboard)
        owner.showReadout (*this);
    repaint();
}

void BoundSlider::focusLost (FocusChangeType)
{
    focusFromKeyboard = false;
    if (! isMouseOver (true))
        owner.hideReadout (*this);
    repaint();
    if (gesture.held())
        suspend();
}

void Knob::updateBounds()
{
    // The pointer turns about its anchor, and the grip ring's frames share theirs.
    auto half = element.hitR + 6.0f;
    for (const auto& key : { element.layer ("pointer"), element.knurl })
        if (key.isNotEmpty())
        {
            const auto& a = owner.ctx.model.asset (key);
            const auto h = key == element.knurl && element.knurlFrames > 0 ? (float) a.image.getHeight() / (float) element.knurlFrames : (float) a.image.getHeight();
            half = std::max ({ half, a.anchor.x, a.anchor.y, (float) a.image.getWidth() - a.anchor.x, h - a.anchor.y });
        }
    auto r = juce::Rectangle<float> (2 * half + 4, 2 * half + 4).withCentre (element.pivot)
                 .getUnion (juce::Rectangle<float> (2 * element.hitR + 16, 2 * element.hitR + 16).withCentre (element.hitCentre()));
    if (! element.hitRect.isEmpty())
        r = r.getUnion (element.hitRect.expanded (2.0f));
    setBounds (r.getSmallestIntegerContainer());
}

void Knob::paint (juce::Graphics& g)
{
    paintPointer (g, owner.ctx.model, element, pointerAngle(), getPosition().toFloat());
    const auto c = element.hitCentre() - getPosition().toFloat();
    if (showsFocusRing())
    {
        g.setColour (accent);
        g.drawEllipse (juce::Rectangle<float> (2 * element.hitR, 2 * element.hitR).withCentre (c), 3.0f);
    }
}

std::optional<std::size_t> Knob::legendAt (Pt local) const
{
    // As far as a lever's legends reach; the skin keeps the legends themselves apart.
    constexpr auto reach = 3.0f;
    if (! element.legendClick)
        return {};
    const auto at = local + getPosition().toFloat();
    std::optional<std::size_t> nearest;
    auto best = reach;
    for (std::size_t i = 0; i < element.positions.size(); ++i)
    {
        const auto& box = element.positions[i].legendRect;
        if (box.isEmpty())
            continue;
        const auto d = at.getDistanceFrom (box.getConstrainedPoint (at));
        if (d <= best && (! nearest || d < best))
        {
            nearest = i;
            best = d;
        }
    }
    return nearest;
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    resumeInput();
    legendPressed = legendAt (e.position);
    if (! legendPressed)
        BoundSlider::mouseDown (e);
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    if (! legendPressed)
        BoundSlider::mouseDrag (e);
}

void Knob::mouseUp (const juce::MouseEvent& e)
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    if (const auto pressed = std::exchange (legendPressed, std::nullopt))
    {
        if (! inputSuspended() && isEnabled() && legendAt (e.position) == pressed)
        {
            setValue (element.positions[*pressed].value, juce::sendNotificationSync);
            owner.showReadout (*this);
        }
        return;
    }
    BoundSlider::mouseUp (e);
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! legendAt (e.position))
        BoundSlider::mouseDoubleClick (e);
}

void Fader::updateBounds()
{
    const auto& a = owner.ctx.model.asset (element.layer ("cap"));
    auto r = element.hitRect.expanded (6.0f);
    const auto top = std::min (element.y0, element.y1), bottom = std::max (element.y0, element.y1);
    r = r.getUnion (juce::Rectangle<float>::leftTopRightBottom (element.capCentreX - a.anchor.x, top - a.anchor.y,
                                                                element.capCentreX - a.anchor.x + (float) a.image.getWidth(),
                                                                bottom - a.anchor.y + (float) a.image.getHeight()));
    for (const auto& [state, key] : element.stateAssets)
    {
        const auto& image = owner.ctx.model.asset (key);
        r = r.getUnion (juce::Rectangle<float>::leftTopRightBottom (element.capCentreX - image.anchor.x, top - image.anchor.y,
                            element.capCentreX - image.anchor.x + (float) image.image.getWidth(),
                            bottom - image.anchor.y + (float) image.image.getHeight()));
    }
    setBounds (r.getSmallestIntegerContainer());
}

void Fader::paint (juce::Graphics& g)
{
    const auto o = getPosition().toFloat();
    const auto y = capY();
    const auto state = element.stateAsset (! isEnabled() ? "disabled" : gestureHeld() ? "pressed" : isMouseOver (true) ? "hover" : "idle");
    if (state.isEmpty())
        paintCap (g, owner.ctx.model, element, proportion(), o);
    else
    {
        const auto& a = owner.ctx.model.asset (state);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImageTransformed (a.image, juce::AffineTransform::translation (element.capCentreX - a.anchor.x - o.x, y - a.anchor.y - o.y));
    }
    if (showsFocusRing())
    {
        g.setColour (accent);
        g.drawRoundedRectangle (juce::Rectangle<float> (36.0f, 50.0f).withCentre ({ element.capCentreX - o.x, y - o.y }), 5.0f, 3.0f);
    }
}

//------------------------------------------------------------------------------ SkinButton

namespace
{
class ButtonAccessibility final : public juce::AccessibilityHandler
{
public:
    explicit ButtonAccessibility (SkinButton& b)
        : AccessibilityHandler (b, b.radio ? juce::AccessibilityRole::radioButton : b.isToggle() ? juce::AccessibilityRole::toggleButton : juce::AccessibilityRole::button,
                                juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [&b] { b.trigger(); })),
          button (b) {}

    juce::AccessibleState getCurrentState() const override
    {
        auto s = AccessibilityHandler::getCurrentState();
        if (button.isToggle() || button.radio)
        {
            s = s.withCheckable();
            if (button.lit())
                s = s.withChecked();
        }
        return s;
    }

    String getTitle() const override { return button.getTitle(); }

private:
    SkinButton& button;
};
} // namespace

SkinButton::SkinButton (View& ownerIn, const Element& el)
    : element (el), owner (ownerIn), panel (ownerIn.panelAround (el.rect)),
      param (el.kind == "toggle" ? &ownerIn.ctx.processor.control (el.paramIndex) : nullptr)
{
    setWantsKeyboardFocus (true);
    setTitle (el.label);
    if (param != nullptr)
    {
        attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float physical)
        {
            value = physical;
            repaint();
        });
        attachment->sendInitialUpdate();
    }
}

void SkinButton::retire()
{
    detail::ParameterListenerDispatch::retire(owner.ctx.processor,attachment);onAction=nullptr;onArrow=nullptr;pressed=false;setEnabled(false);
}

void SkinButton::updateBounds()
{
    auto r = element.rect.expanded (8.0f);
    if (const auto layer = element.layer ("lit"); layer.isNotEmpty())
    {
        const auto& a = owner.ctx.model.asset (layer);
        const auto s = a.lensR > 0.0f ? element.lensR / a.lensR : 1.0f;
        r = r.getUnion (juce::Rectangle<float> ((float) a.image.getWidth() * s, (float) a.image.getHeight() * s)
                            .withPosition (element.led - a.anchor * s));
    }
    for (const auto& [state, key] : element.stateAssets)
    {
        const auto& a = owner.ctx.model.asset (key);
        r = r.getUnion (juce::Rectangle<float> ((float) a.image.getWidth() * element.stateScale.x, (float) a.image.getHeight() * element.stateScale.y)
                            .withPosition (element.rect.getCentre() - a.anchor * element.stateScale));
    }
    setBounds (r.getSmallestIntegerContainer());
}

void SkinButton::paint (juce::Graphics& g)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    const auto o = getPosition().toFloat();
    const auto face = area() - o;
    if (! element.stateAssets.empty())
    {
        const auto resting = isToggle() ? (lit() ? String ("on") : String ("off"))
                                       : lit() && element.stateAsset ("selected").isNotEmpty() ? String ("selected") : String ("idle");
        const auto phase = ! isEnabled() ? String ("disabled") : pressed && pressInside ? String ("pressed") : hover ? String ("hover") : String();
        const auto state = phase.isEmpty() ? resting : isToggle() && phase != "disabled" ? phase + "_" + resting : phase;
        const auto& a = owner.ctx.model.asset (element.stateAsset (state));
        const auto at = element.rect.getCentre() - o;
        g.drawImageTransformed (a.image, juce::AffineTransform::translation (-a.anchor.x, -a.anchor.y)
                                .scaled (element.stateScale.x, element.stateScale.y).translated (at.x, at.y));
        g.setColour (juce::Colour (0xff33252a));
        g.setFont (juce::Font (juce::FontOptions ("Bahnschrift", isToggle() ? 35.0f : 31.0f, juce::Font::bold)));
        const auto caption = element.stateCaptionAt
            ? juce::Rectangle<float> (element.stateCaptionAt->x - o.x, element.stateCaptionAt->y - o.y, element.rect.getWidth(), 48)
            : element.rect - o;
        const auto rendered = owner.ctx.generatedText != nullptr
            && owner.ctx.generatedText->draw (g, getTitle(), g.getCurrentFont(), juce::Colour (0xff33252a), caption, ! element.stateCaptionAt);
        if (! rendered && element.stateCaptionAt)
            g.drawText (getTitle(), juce::Rectangle<float> (element.stateCaptionAt->x - o.x - 1, element.stateCaptionAt->y - o.y - 1,
                                                          element.rect.getWidth(), 48), juce::Justification::topLeft, false);
        else if (! rendered)
            g.drawText (getTitle(), (element.rect - o).translated (0, -2), juce::Justification::centred, false);
        if (showsFocusRing())
        {
            g.setColour (accent);
            g.drawRoundedRectangle (face.expanded (3.0f), 5.0f, 2.0f);
        }
        return;
    }
    const auto light = [&]
    {
        g.setOpacity (isEnabled() ? 1.0f : 0.35f);
        paintLight (g, owner.ctx.model, element, o);
        g.setOpacity (1.0f);
    };
    // A lit place of its own lies under the hover and press it takes there; an LED's light over them.
    const auto litPlace = lit() && ! element.litRect.isEmpty();
    if (litPlace)
        light();
    if (! isEnabled())
    {
        g.setColour (panel.withAlpha (0.7f));
        g.fillRoundedRectangle (face.expanded (2.0f), 5.0f);
    }
    else if (pressed && pressInside)
    {
        g.setColour (juce::Colours::black.withAlpha (0.30f));
        g.fillRoundedRectangle (face.reduced (2.5f), 3.5f);
    }
    else if (hover)
    {
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRoundedRectangle (face.reduced (2.5f), 3.5f);
    }
    if (lit() && ! litPlace)
        light();
    if (showsFocusRing())
    {
        g.setColour (accent);
        g.drawRoundedRectangle (face.expanded (3.0f), 5.0f, 2.0f);
    }
}

void SkinButton::mouseDown (const juce::MouseEvent&)
{
    if (! isEnabled())
        return;
    pressed = pressInside = true;
    repaint();
}

void SkinButton::mouseDrag (const juce::MouseEvent& e)
{
    const auto inside = hitTest (juce::roundToInt (e.position.x), juce::roundToInt (e.position.y));
    if (pressed && inside != pressInside)
    {
        pressInside = inside;
        repaint();
    }
}

void SkinButton::mouseUp (const juce::MouseEvent& e)
{
    const auto activate = pressed && hitTest (juce::roundToInt (e.position.x), juce::roundToInt (e.position.y));
    pressed = pressInside = false;
    repaint();
    if (activate)
        trigger();
}

bool SkinButton::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::spaceKey || key.getKeyCode() == juce::KeyPress::returnKey)
    {
        trigger();
        return true;
    }
    if (onArrow && (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey))
    {
        onArrow (key.getKeyCode() == juce::KeyPress::leftKey ? -1 : 1);
        return true;
    }
    return false;
}

void SkinButton::trigger()
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    if (! isEnabled())
        return;
    if (isToggle())
    {
        write (lit() ? 0.0f : (float) element.onIndex);
        owner.ctx.touch (element);
    }
    else if (onAction)
        onAction();
}

void SkinButton::write (float physical)
{
    auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    if (owner.ctx.closing())
        return;
    auto& processor = owner.ctx.processor;
    const auto index = element.paramIndex;
    if (! processor.beginParameterGesture (index))
    {
        reject();
        return;
    }
    const bool ok = ! owner.ctx.closing() && processor.setParameterGestureValue (index, param->convertTo0to1 (physical));
    const bool ended = processor.endParameterGesture (index);
    if ((! ok || ! ended) && ! owner.ctx.closing())
        reject();
}

void SkinButton::reject()
{
    value = param->convertFrom0to1 (param->getValue());
    repaint();
    owner.ctx.report (rejectedEdit);
}

std::unique_ptr<juce::AccessibilityHandler> SkinButton::createAccessibilityHandler()
{
    return std::make_unique<ButtonAccessibility> (*this);
}

//------------------------------------------------------------------------------ the moving layers

void paintNativeEngraving (juce::Graphics& g, const Model& m, const Element& e, Pt origin)
{
    if (! e.nativeEngraving)
        return;
    const auto& n = *e.nativeEngraving;
    juce::Graphics::ScopedSaveState saved (g);
    if (e.nativeAlpha < 1.0f)
        g.beginTransparencyLayer (e.nativeAlpha);
    g.addTransform (juce::AffineTransform::translation (-origin.x, -origin.y));
    const auto at = [&e] (float angle, float radius)
    {
        const auto a = juce::degreesToRadians (angle);
        return e.pivot + Pt (std::sin (a), -std::cos (a)) * radius;
    };
    g.setColour (n.ink);
    const auto radius = e.kind == "selector" ? e.faceR + 11.0f : e.tickR;
    const auto tick = [&] (float angle, float length)
    {
        g.drawLine (juce::Line<float> (at (angle, radius), at (angle, radius + length)), 1.5f);
    };
    if (e.kind == "rotary")
    {
        if (n.minorStep > 0)
        {
            const auto span = (float) (e.sweepHi - e.sweepLo);
            const auto count = std::min (360, (int) std::ceil (span / n.minorStep));
            for (int i = 0; i <= count; ++i)
                tick ((float) e.sweepLo + std::min (span, (float) i * n.minorStep), n.minorLength);
        }
        for (const auto& [value, ignoredAngle] : e.ticks)
        {
            juce::ignoreUnused (ignoredAngle);
            tick (pointerAngleFor (e, proportionOf (e, value)), n.majorLength);
        }
    }
    else
        for (const auto& position : e.positions)
            tick (position.angle, n.majorLength);
    const auto text = [&] (const String& words, juce::Rectangle<float> rect, float fontHeight)
    {
        if (words.isEmpty() || rect.isEmpty() || m.nativeEngravingFace == nullptr)
            return;
        g.setFont (juce::Font (juce::FontOptions (m.nativeEngravingFace).withHeight (fontHeight)));
        g.drawFittedText (words, rect.toNearestInt(), juce::Justification::centred, 1, 0.8f);
    };
    text (e.label, n.labelRect, n.labelFontHeight);
    text (e.unit, n.unitRect, n.scaleFontHeight);
    if (e.kind == "selector")
        for (const auto& position : e.positions)
            text (position.legend, position.legendRect, n.scaleFontHeight);
    else
        for (const auto& [value, words] : n.tickLabels)
        {
            const auto angle = pointerAngleFor (e, proportionOf (e, value));
            auto centre = at (angle, n.labelRadius);
            if (std::abs (angle) > 90.0f) centre.x += angle < 0 ? n.labelInset : -n.labelInset;
            text (words, juce::Rectangle<float> (72.0f, 26.0f).withCentre (centre), n.scaleFontHeight);
        }
    if (e.nativeAlpha < 1.0f)
        g.endTransparencyLayer();
}

void paintNineSlice (juce::Graphics& g, const Asset& a, juce::Rectangle<int> area)
{
    if(area.isEmpty())return;
    const auto& src=a.image;const auto l=a.slice[0],t=a.slice[1],r=a.slice[2],b=a.slice[3];
    const int xs[]{0,l,src.getWidth()-r,src.getWidth()},ys[]{0,t,src.getHeight()-b,src.getHeight()};
    const int xd[]{area.getX(),area.getX()+l,area.getRight()-r,area.getRight()},yd[]{area.getY(),area.getY()+t,area.getBottom()-b,area.getBottom()};
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    for(int y=0;y<3;++y)for(int x=0;x<3;++x)
        g.drawImage(src,xd[x],yd[y],xd[x+1]-xd[x],yd[y+1]-yd[y],xs[x],ys[y],xs[x+1]-xs[x],ys[y+1]-ys[y]);
}

void paintNativeViewText (juce::Graphics& g, const Model& m, const String& view, Pt origin)
{
    if (! m.nativeFace || view != "setup" || m.nativeEngravingFace == nullptr) return;
    juce::Graphics::ScopedSaveState saved (g);
    g.addTransform (juce::AffineTransform::translation (-origin.x, -origin.y));
    g.setColour (juce::Colour (0xfff0d2a3));
    g.setFont (juce::Font (juce::FontOptions (m.nativeEngravingFace).withHeight (25.0f)));
    for (const auto& e : m.elements)
        if (e.view == view && e.localId() == "all-controls")
            g.drawText ("ALL 36 CONTROLS", e.rect, juce::Justification::centred);
}

void paintPointer (juce::Graphics& g, const Model& m, const Element& e, float angle, Pt origin)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    const auto c = e.pivot - origin;
    if (e.knurl.isNotEmpty() && e.knurlFrames > 0)
    {
        // The ring stays put and shows the frame whose teeth stand where the turned knob's would.
        const auto& k = m.asset (e.knurl);
        const auto h = k.image.getHeight() / e.knurlFrames;
        const auto frame = k.image.getClippedImage ({ 0, gripFrame (e, angle) * h, k.image.getWidth(), h });
        g.drawImageTransformed (frame, juce::AffineTransform::translation (c.x - k.anchor.x, c.y - k.anchor.y));
    }
    const auto& a = m.asset (e.layer ("pointer"));
    g.drawImageTransformed (a.image, juce::AffineTransform::translation (-a.anchor.x, -a.anchor.y)
                                         .rotated (juce::degreesToRadians (angle))
                                         .translated (c.x, c.y));
}

void paintBat (juce::Graphics& g, const Model& m, const Element& e, const Position& p, Pt origin)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    const auto& a = m.asset (p.asset);
    const auto at = e.pivot - a.anchor - origin;
    g.drawImageTransformed (a.image, juce::AffineTransform::translation (at.x, at.y));
}

void paintCap (juce::Graphics& g, const Model& m, const Element& e, double proportion, Pt origin)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    const auto& a = m.asset (e.layer ("cap"));
    const auto y = Fader::capYAt (e, proportion);
    g.drawImageTransformed (a.image, juce::AffineTransform::translation (e.capCentreX - a.anchor.x - origin.x, y - a.anchor.y - origin.y));
}

void paintLight (juce::Graphics& g, const Model& m, const Element& e, Pt origin)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    const auto& a = m.asset (e.layer ("lit"));
    const auto s = a.lensR > 0.0f ? e.lensR / a.lensR : 1.0f;
    const auto c = e.led - origin;
    g.drawImageTransformed (a.image, juce::AffineTransform::translation (-a.anchor.x, -a.anchor.y).scaled (s).translated (c.x, c.y));
}

void paintNeedle (juce::Graphics& g, const Model& m, const Element& e, double proportion, Pt origin)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    const auto& a = m.asset (e.layer ("needle"));
    const auto at = Pt (pointerAngleFor (e, proportion), e.y0) - a.anchor - origin;
    g.drawImageTransformed (a.image, juce::AffineTransform::translation (at.x, at.y));
}

const Position& positionFor (const Element& e, double value)
{
    jassert (! e.positions.empty());
    return *std::min_element (e.positions.begin(), e.positions.end(),
                              [value] (const Position& a, const Position& b) { return std::abs (a.value - value) < std::abs (b.value - value); });
}

//------------------------------------------------------------------------------ Lever, Lamp

namespace
{
class LeverAccessibility final : public juce::AccessibilityHandler
{
public:
    explicit LeverAccessibility (Lever& l)
        : AccessibilityHandler (l, juce::AccessibilityRole::button,
                                juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [&l] { l.trigger(); }),
                                Interfaces { std::make_unique<Value> (l) }),
          lever (l) {}

    String getTitle() const override { return lever.element.label; }

private:
    /** The printed legend of the lever's position: IN or OUT, TANH or SOFT. */
    struct Value final : public juce::AccessibilityTextValueInterface
    {
        explicit Value (Lever& l) : lever (l) {}
        bool isReadOnly() const override { return true; }
        String getCurrentValueAsString() const override { return lever.legend(); }
        void setValueAsString (const String&) override {}
        Lever& lever;
    };

    Lever& lever;
};

juce::Rectangle<float> spriteBounds (const Asset& a, Pt at, float scale = 1.0f)
{
    return juce::Rectangle<float> ((float) a.image.getWidth() * scale, (float) a.image.getHeight() * scale).withPosition (at - a.anchor * scale);
}
} // namespace

Lever::Lever (View& ownerIn, const Element& el)
    : element (el), owner (ownerIn), param (ownerIn.ctx.processor.control (el.paramIndex)),
      gesture (ownerIn.ctx.processor, el.paramIndex, ownerIn.ctx.host, [this] (float physical) { read (physical); })
{
    setWantsKeyboardFocus (true);
    setTitle (el.label);
    attachment = std::make_unique<juce::ParameterAttachment> (param, [this] (float physical) { read (physical); });
    attachment->sendInitialUpdate();
}

Lever::~Lever() { attachment.reset(); }

void Lever::retire()
{
    gesture.retire();detail::ParameterListenerDispatch::retire(owner.ctx.processor,attachment);pressed=false;setEnabled(false);
}

void Lever::read (float physical)
{
    value = physical;
    repaint();
}

void Lever::updateBounds()
{
    auto r = area();
    if (isSelector())
    {
        // The pointer turns about its anchor.
        const auto& a = owner.ctx.model.asset (element.layer ("pointer"));
        const auto half = std::max ({ element.hitR, a.anchor.x, a.anchor.y, (float) a.image.getWidth() - a.anchor.x, (float) a.image.getHeight() - a.anchor.y });
        r = r.getUnion (juce::Rectangle<float> (2 * half, 2 * half).withCentre (element.pivot));
    }
    else
        for (const auto& p : element.positions)
            r = r.getUnion (spriteBounds (owner.ctx.model.asset (p.asset), element.pivot));
    setBounds (r.expanded (4.0f).getSmallestIntegerContainer());
}

void Lever::paint (juce::Graphics& g)
{
    const auto o = getPosition().toFloat();
    if (isSelector())
    {
        paintPointer (g, owner.ctx.model, element, position().angle, o);
        const auto c = element.pivot - o;
        if (hover && isEnabled())
        {
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillEllipse (juce::Rectangle<float> (2 * element.faceR, 2 * element.faceR).withCentre (c));
        }
        if (showsFocusRing())
        {
            g.setColour (accent);
            g.drawEllipse (juce::Rectangle<float> (2 * element.hitR, 2 * element.hitR).withCentre (c), 3.0f);
        }
        return;
    }
    paintBat (g, owner.ctx.model, element, position(), o);
    const auto face = element.rect - o;
    if (hover && isEnabled() && ! element.legendClick)
    {
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRoundedRectangle (face.reduced (2.5f), 3.5f);
    }
    if (showsFocusRing())
    {
        g.setColour (accent);
        g.drawRoundedRectangle (face.expanded (3.0f), 5.0f, 2.0f);
    }
}

void Lever::mouseUp (const juce::MouseEvent& e)
{
    if (! std::exchange (pressed, false) || ! hitTest (juce::roundToInt (e.position.x), juce::roundToInt (e.position.y)))
        return;
    if (isSelector() || element.legendClick)
    {
        const auto inView = e.position + getPosition().toFloat();
        for (std::size_t i = 0; i < element.positions.size(); ++i)
            if (! element.positions[i].legendRect.isEmpty() && element.positions[i].legendRect.expanded (3.0f).contains (inView))
                return choose (i);
        if (! isSelector())
            return;
    }
    trigger();
}

void Lever::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (w.deltaY == 0.0f || w.isInertial || rackScrolling())
        return juce::Component::mouseWheelMove (e, w);
    step (w.deltaY > 0.0f ? 1 : -1);
}

bool Lever::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::spaceKey || code == juce::KeyPress::returnKey)
        trigger();
    else if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)
        step (1);
    else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)
        step (-1);
    else
        return false;
    return true;
}

void Lever::step (int by)
{
    if (element.positions.empty())
        return;
    // A lever's positions run from its bat's top down, so a step up is back towards the first.
    const auto to = juce::jlimit (0, (int) element.positions.size() - 1, (int) at() + (isSelector() ? by : -by));
    choose ((std::size_t) to);
}

void Lever::choose (std::size_t index)
{
    const auto frame = owner.ctx.frame();
    const detail::ParameterListenerDispatch dispatch(owner.ctx.processor);
    if (! isEnabled() || index >= element.positions.size() || index == at())
        return;
    gesture.write ((float) element.positions[index].value);
    owner.ctx.touch (element);
}

void Lever::trigger()
{
    if (! isEnabled() || element.positions.empty())
        return;
    choose ((at() + 1) % element.positions.size());
}

std::unique_ptr<juce::AccessibilityHandler> Lever::createAccessibilityHandler()
{
    return std::make_unique<LeverAccessibility> (*this);
}

Lamp::Lamp (View& ownerIn, const Element& el) : element (el), owner (ownerIn)
{
    setInterceptsMouseClicks (false, false);
    attachment = std::make_unique<juce::ParameterAttachment> (ownerIn.ctx.processor.control (el.paramIndex), [this] (float physical)
    {
        value = physical;
        repaint();
    });
    attachment->sendInitialUpdate();
}

Lamp::~Lamp() { attachment.reset(); }

void Lamp::retire(){detail::ParameterListenerDispatch::retire(owner.ctx.processor,attachment);setEnabled(false);}

void Lamp::updateBounds()
{
    const auto& a = owner.ctx.model.asset (element.layer ("lit"));
    setBounds (spriteBounds (a, element.led, a.lensR > 0.0f ? element.lensR / a.lensR : 1.0f).expanded (2.0f).getSmallestIntegerContainer());
}

void Lamp::paint (juce::Graphics& g)
{
    if (lit())
        paintLight (g, owner.ctx.model, element, getPosition().toFloat());
}

Needle::Needle (View& ownerIn, const Element& el) : element (el), owner (ownerIn)
{
    setInterceptsMouseClicks (false, false);
    attachment = std::make_unique<juce::ParameterAttachment> (ownerIn.ctx.processor.control (el.paramIndex), [this] (float physical)
    {
        const auto was = layerAt (x());
        value = physical;
        repaint (was - getPosition());
        repaint (layerAt (x()) - getPosition());
    });
    attachment->sendInitialUpdate();
}

Needle::~Needle() { attachment.reset(); }

void Needle::retire(){detail::ParameterListenerDispatch::retire(owner.ctx.processor,attachment);setEnabled(false);}

float Needle::x() const { return pointerAngleFor (element, proportionOf (element, element.range.clipValue ((double) value))); }

juce::Rectangle<int> Needle::layerAt (float at) const
{
    const auto& a = owner.ctx.model.asset (element.layer ("needle"));
    return juce::Rectangle<float> ((float) a.image.getWidth(), (float) a.image.getHeight()).withPosition (at - a.anchor.x, element.y0 - a.anchor.y)
        .getSmallestIntegerContainer()
        .expanded (1);
}

void Needle::updateBounds()
{
    const auto ends = element.ticks.empty() ? std::pair (0.0f, 0.0f) : std::pair (element.ticks.front().second, element.ticks.back().second);
    setBounds (layerAt (ends.first).getUnion (layerAt (ends.second)));
}

void Needle::paint (juce::Graphics& g)
{
    paintNeedle (g, owner.ctx.model, element, proportionOf (element, element.range.clipValue ((double) value)), getPosition().toFloat());
}

//------------------------------------------------------------------------------ Tape

static juce::Font tapeFont (const Lettering& l, float height) { return juce::Font (juce::FontOptions (l.face).withHeight (height)); }

struct TapeInk
{
    juce::Path path;
    float cap = 0.0f;
};

static juce::Path linePath (const juce::Font& f, const String& s)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (f, s, 0.0f, 0.0f);
    juce::Path p;
    ga.createPath (p);
    return p;
}

/** The marker's font height at which its cap is `cap` px. */
static float heightForCap (const Lettering& l, float cap) { return 100.0f * cap / linePath (tapeFont (l, 100.0f), "H").getBounds().getHeight(); }

/** Marker text centred on a tape body of `size` (origin = body centre): one line, or split at the space nearest the
    middle onto two lines when `twoLines`. */
static TapeInk tapeInk (const String& text, Pt size, bool twoLines, const Lettering& l)
{
    juce::StringArray lines;
    if (twoLines && text.containsChar (' '))
    {
        auto best = -1;
        for (int i = 0; i < text.length(); ++i)
            if (text[i] == ' ' && (best < 0 || std::abs (i - text.length() / 2) < std::abs (best - text.length() / 2)))
                best = i;
        lines.add (text.substring (0, best).trim());
        lines.add (text.substring (best + 1).trim());
    }
    else
        lines.add (text);

    auto cap = size.y * (lines.size() == 2 ? l.capTwoLines : l.capOneLine);
    auto height = heightForCap (l, cap);
    auto widest = 0.0f;
    for (auto& line : lines)
        widest = std::max (widest, linePath (tapeFont (l, height), line).getBounds().getWidth());
    if (widest > l.maxWidthOfBody * size.x)
    {
        const auto k = l.maxWidthOfBody * size.x / widest;
        height *= k;
        cap *= k;
    }
    const auto pitch = l.pitchOfCap * cap;
    const auto total = cap + (float) (lines.size() - 1) * pitch;
    juce::Path out;
    for (int i = 0; i < lines.size(); ++i)
    {
        const auto p = linePath (tapeFont (l, height), lines[i]);
        out.addPath (p, juce::AffineTransform::translation (-p.getBounds().getCentreX(), -total / 2.0f + cap + (float) i * pitch));
    }
    return { out, cap };
}

static juce::Image nineSlice (const Asset& a, int w, int h)
{
    const auto& src = a.image;
    const int l = a.slice[0], t = a.slice[1], r = a.slice[2], b = a.slice[3];
    const int sw = src.getWidth(), sh = src.getHeight();
    juce::Image out (juce::Image::ARGB, w, h, true);
    juce::Graphics g (out);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    const int xs[] = { 0, l, sw - r, sw }, ys[] = { 0, t, sh - b, sh };
    const int xd[] = { 0, l, w - r, w }, yd[] = { 0, t, h - b, h };
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            g.drawImage (src, xd[i], yd[j], xd[i + 1] - xd[i], yd[j + 1] - yd[j], xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j]);
    return out;
}

TapeArt::TapeArt (const Model& m, const String& blankAsset, Pt s) : model (m), asset (blankAsset), size (s)
{
    const auto& a = model.asset (asset);
    const auto pads = a.image.getWidth() - a.body.getWidth();
    const auto padsY = a.image.getHeight() - a.body.getHeight();
    blank = nineSlice (a, juce::roundToInt (size.x) + pads, juce::roundToInt (size.y) + padsY);
}

float TapeArt::radius() const { return std::hypot ((float) blank.getWidth(), (float) blank.getHeight()) / 2.0f; }

float TapeArt::widthFor (const Model& m, const String& text, float height)
{
    const auto& l = m.lettering;
    return linePath (tapeFont (l, heightForCap (l, height * l.capOneLine)), text).getBounds().getWidth() / l.maxWidthOfBody;
}

void TapeArt::paint (juce::Graphics& g, Pt centre, float angle, const String& text, bool twoLines, std::optional<juce::Colour> tint) const
{
    const auto& a = model.asset (asset);
    const auto place = juce::AffineTransform::rotation (juce::degreesToRadians (angle)).translated (centre.x, centre.y);
    const auto body = Pt ((float) a.body.getX() + size.x / 2.0f, (float) a.body.getY() + size.y / 2.0f);
    const auto at = juce::AffineTransform::translation (-body.x, -body.y).followedBy (place);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    if (tint)
    {
        g.setColour (*tint);
        g.drawImageTransformed (blank, at, true);
    }
    g.setOpacity (tint ? 0.3f : 1.0f);
    g.drawImageTransformed (blank, at);
    const auto& l = model.lettering;
    const auto ink = tapeInk (text, size, twoLines, l);
    g.beginTransparencyLayer (l.alpha);
    g.setColour (l.ink);
    g.fillPath (ink.path, place);
    if (l.strokeOfCap > 0.0f)
        g.strokePath (ink.path, juce::PathStrokeType (l.strokeOfCap * ink.cap, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), place);
    g.endTransparencyLayer();
}

Tape::Tape (View& ownerIn, const Element& el) : element (el), owner (ownerIn), art (ownerIn.ctx.model, el.layer ("blank"), el.size)
{
    setInterceptsMouseClicks (false, false);
}

String Tape::text() const { return owner.ctx.tapeText ? owner.ctx.tapeText (element) : String(); }

void Tape::updateBounds()
{
    const auto r = art.radius() + 4.0f;
    setBounds (juce::Rectangle<float> (2 * r, 2 * r).withCentre (element.centre).getSmallestIntegerContainer());
}

void Tape::paint (juce::Graphics& g)
{
    const auto o = getPosition().toFloat();
    const auto t = text();
    if (t.isEmpty())
    {
        if (element.hasDash)
        {
            g.setColour (juce::Colour (0xffebebeb));
            g.fillRoundedRectangle (element.dash - o, 1.0f);
        }
        return;
    }
    art.paint (g, element.centre - o, element.angle, t, element.localId().startsWith ("slot-tape"));
}

//------------------------------------------------------------------------------ View

View::View (Context& c, const String& viewName, const String& idPrefix) : ctx (c), name (viewName)
{
    const auto& a = ctx.model.asset (viewName + ":plate");
    plate = a.image;
    plateAt = a.anchor.roundToInt();
    setSize (plateAt.x + plate.getWidth(), plateAt.y + plate.getHeight());
    setComponentID (idPrefix + viewName);
    // Not a keyboard focus container: Tab reaches its controls from the window's.
    setFocusContainerType (FocusContainerType::focusContainer);
    for (const auto& gr : ctx.model.groups)
        if (gr.view == viewName)
            focusOrder.addArray (gr.focusOrder);

    for (const auto& e : ctx.model.elements)
    {
        if (e.view != viewName)
            continue;
        juce::Component* comp = nullptr;
        if (e.kind == "rotary" || e.kind == "fader")
        {
            std::unique_ptr<BoundSlider> s;
            if (e.kind == "rotary") s = std::make_unique<Knob> (*this, e);
            else                    s = std::make_unique<Fader> (*this, e);
            s->updateBounds();
            comp = s.get();
            sliders.push_back (std::move (s));
        }
        else if (e.kind == "toggle" || e.kind == "action")
        {
            auto b = std::make_unique<SkinButton> (*this, e);
            b->updateBounds();
            comp = b.get();
            buttons.push_back (std::move (b));
        }
        else if (e.kind == "tape" && e.layer ("blank").isNotEmpty())
        {
            auto t = std::make_unique<Tape> (*this, e);
            t->updateBounds();
            addAndMakeVisible (*t);
            tapes.push_back (std::move (t));
        }
        else if (e.kind == "lever" || e.kind == "selector")
        {
            auto l = std::make_unique<Lever> (*this, e);
            l->updateBounds();
            comp = l.get();
            levers.push_back (std::move (l));
        }
        else if (e.kind == "lamp")
        {
            auto l = std::make_unique<Lamp> (*this, e);
            l->updateBounds();
            addAndMakeVisible (*l);
            lamps.push_back (std::move (l));
        }
        else if (e.kind == "needle")
        {
            auto n = std::make_unique<Needle> (*this, e);
            n->updateBounds();
            addAndMakeVisible (*n);
            needles.push_back (std::move (n));
        }
        if (comp != nullptr)
        {
            comp->setComponentID (idPrefix + e.id);
            comp->getProperties().set ("element", e.id);
            if (const auto idx = focusOrder.indexOf (e.id); idx >= 0)
                comp->setExplicitFocusOrder (idx + 1);
            else
                comp->setWantsKeyboardFocus (false);
            addAndMakeVisible (comp);
        }
    }
    addChildComponent (readout);
}

void View::retire()
{
    for(auto& control:sliders)control->retire();
    for(auto& control:buttons)control->retire();
    for(auto& control:levers)control->retire();
    for(auto& control:lamps)control->retire();
    for(auto& control:needles)control->retire();
}

View::~View()
{
    onGestureEnd = nullptr;
    // The controls end held gestures as they go, and may show the parameter again; the rest of the view outlives them.
    sliders.clear();
    buttons.clear();
    levers.clear();
    lamps.clear();
    needles.clear();
    tapes.clear();
}

void View::hideReadout (BoundSlider& s)
{
    if (active == &s)
    {
        active = nullptr;
        readout.setVisible (false);
    }
}

void View::suspendInputs()
{
    const auto scope = ctx.frame();
    std::vector<juce::Component::SafePointer<BoundSlider>> controls;
    for (const auto& slider : sliders)
        controls.emplace_back (slider.get());
    for (const auto& slider : controls)
        if (slider != nullptr)
            slider->suspend();
}

bool View::hasHeldGesture() const
{
    return std::any_of (sliders.begin(), sliders.end(), [] (const auto& slider) { return slider->gestureHeld(); });
}

void View::hideReadouts()
{
    active = nullptr;
    readout.setVisible (false);
}

void View::refreshReadout()
{
    if (active == nullptr || ! ctx.readouts)
        return;
    readout.show (active->element.label + "  " + active->readoutText(), active->readoutBeside(), getLocalBounds().reduced (4));
    if (ctx.model.nativeFace)
        readout.setTopLeftPosition (juce::jlimit (430, 1790 - readout.getWidth(), juce::roundToInt (active->element.pivot.x) - readout.getWidth() / 2), 6);
    readout.toFront (false);
}

void View::elementChanged (BoundSlider& s)
{
    if (ctx.valueShown)
        ctx.valueShown (s.element);
    if (active == &s)
        refreshReadout();
}

void View::refreshTapes()
{
    for (auto& t : tapes)
        t->repaint();
}

juce::Colour View::panelAround (juce::Rectangle<float> r) const
{
    r -= plateAt.toFloat();
    const auto outer = r.expanded (9.0f).getSmallestIntegerContainer().getIntersection (plate.getBounds());
    const auto inner = r.expanded (5.0f);
    const juce::Image::BitmapData bd (plate, juce::Image::BitmapData::readOnly);
    std::vector<juce::uint8> channel[3];
    for (int y = outer.getY(); y < outer.getBottom(); ++y)
        for (int x = outer.getX(); x < outer.getRight(); ++x)
            if (! inner.contains ((float) x + 0.5f, (float) y + 0.5f))
            {
                const auto c = bd.getPixelColour (x, y);
                channel[0].push_back (c.getRed());
                channel[1].push_back (c.getGreen());
                channel[2].push_back (c.getBlue());
            }
    if (channel[0].empty())
        return juce::Colour (0xff303236);
    juce::uint8 m[3];
    for (int i = 0; i < 3; ++i)
    {
        auto& v = channel[i];
        std::nth_element (v.begin(), v.begin() + (std::ptrdiff_t) (v.size() / 2), v.end());
        m[i] = v[v.size() / 2];
    }
    return juce::Colour (m[0], m[1], m[2]);
}

//------------------------------------------------------------------------------ AssignEditor

AssignEditor::AssignEditor()
{
    setLookAndFeel (&look);
    setComponentID ("house.assign");
    setTitle ("Macro assignment");
    target.setComponentID ("house.assign.target");
    target.setTitle ("Macro target");
    depth.setComponentID ("house.assign.depth");
    depth.setSliderStyle (juce::Slider::LinearHorizontal);
    depth.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 40);
    depth.setRange (-100.0, 100.0, 0.1);
    depth.setTitle ("Macro depth");
    clear.setComponentID ("house.assign.clear");
    done.setComponentID ("house.assign.close");
    target.onChange = [this] { push(); };
    depth.onDragStart = [this] { dragging = true; };
    depth.onDragEnd = [this]
    {
        if (! std::exchange (dragging, false))
            return;
        const auto n = macro;
        push();
        if (n != 0 && onPreview)
            onPreview (n, std::nullopt);
    };
    depth.onValueChange = [this]
    {
        if (! dragging)
            push();
        else if (! loading && macro != 0 && onPreview)
            onPreview (macro, assignment());
    };
    clear.onClick = [this] { target.setSelectedId (1, juce::sendNotificationSync); };
    done.onClick = [this] { close(); };
    for (auto* comp : std::initializer_list<juce::Component*> { &target, &depth, &clear, &done })
        addAndMakeVisible (comp);
    setRackTargets ({});
    setSize (520, 196);
}

AssignEditor::~AssignEditor()
{
    // juce::Slider ends a held drag as it goes, after the rest of the editor has gone.
    depth.onDragStart = depth.onDragEnd = depth.onValueChange = nullptr;
    setLookAndFeel (nullptr);
}

void AssignEditor::open (int macroIndex, juce::Component& from, juce::Rectangle<float> column)
{
    if (macro != 0)
        close (false);
    macro = macroIndex;
    opener = &from;
    if (auto* p = getParentComponent())
    {
        const auto limits = p->getLocalBounds().reduced (4);
        // 64 px on the left clears the ASSIGN legend printed beside the first button of each row
        auto r = getLocalBounds().withCentre (column.getCentre().toInt()).withRightX (juce::roundToInt (column.getX()) - 64);
        if (r.getX() < limits.getX())
            r.setX (juce::roundToInt (column.getRight()) + 14);
        setBounds (r.constrainedWithin (limits));
    }
    setVisible (true);
    toFront (false);
    target.grabKeyboardFocus();
    if (onOpenChanged)
        onOpenChanged (macro, true);
    repaint();
}

void AssignEditor::close (bool restoreFocus)
{
    if (macro == 0)
        return;
    const auto n = macro;
    macro = 0;
    if (std::exchange (dragging, false) && onPreview)
        onPreview (n, std::nullopt);
    setVisible (false);
    if (onOpenChanged)
        onOpenChanged (n, false);
    if (restoreFocus && opener != nullptr && opener->isShowing())
        opener->grabKeyboardFocus();
    opener = nullptr;
}

void AssignEditor::show (const Assignment& a)
{
    const juce::ScopedValueSetter<bool> quiet (loading, true);
    target.setSelectedId (idFor (a.target), juce::dontSendNotification);
    depth.setValue (a.target ? a.amount * 100.0 : 0.0, juce::dontSendNotification);
    depth.setEnabled (a.target.has_value());
}

void AssignEditor::setRackTargets (const std::vector<RackTarget>& targets)
{
    const auto shown = assignment();
    const juce::ScopedValueSetter<bool> quiet (loading, true);
    target.clear (juce::dontSendNotification);
    items.clear();
    target.addItem ("None (unassigned)", 1);
    for (const auto& t : targets)
    {
        items.push_back (t.index);
        target.addItem (t.label + "   " + t.rack, (int) items.size() + 1);
    }
    const auto registry = parameterRegistry();
    for (std::size_t n = 0; n < registry.size(); ++n)
        if (registry[n].kind == ParameterKind::continuous && (n == 0 || n >= 9))
        {
            items.push_back (n);
            target.addItem (registry[n].name, (int) items.size() + 1);
        }
    target.setSelectedId (idFor (shown.target), juce::dontSendNotification);
}

Assignment AssignEditor::assignment() const
{
    const auto id = target.getSelectedId();
    if (id < 2 || id - 2 >= (int) items.size())
        return {};
    return { items[(size_t) id - 2], (float) (depth.getValue() / 100.0) };
}

int AssignEditor::idFor (std::optional<std::size_t> index) const
{
    if (index)
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i] == *index)
                return (int) i + 2;
    return 1;
}

void AssignEditor::push()
{
    if (loading || macro == 0)
        return;
    const auto a = assignment();
    if (! a.target)
    {
        const juce::ScopedValueSetter<bool> quiet (loading, true);
        depth.setValue (0.0, juce::dontSendNotification);
    }
    depth.setEnabled (a.target.has_value());
    if (onAssign)
        onAssign (macro, a);
}

void AssignEditor::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (juce::Colour (0xf4141518));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (accent);
    g.drawRoundedRectangle (r, 8.0f, 2.0f);
    g.setColour (juce::Colour (0xfff2f2ee));
    g.setFont (juce::Font (juce::FontOptions (26.0f, juce::Font::bold)));
    g.drawText ("MACRO " + String (macro) + "  ASSIGN", 20, 12, getWidth() - 40, 32, juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xff9a9ca3));
    g.setFont (juce::Font (juce::FontOptions (22.0f)));
    g.drawText ("Depth", 20, 112, 90, 40, juce::Justification::centredLeft);
}

void AssignEditor::resized()
{
    target.setBounds (20, 56, getWidth() - 40, 44);
    depth.setBounds (110, 112, getWidth() - 130, 40);
    clear.setBounds (getWidth() - 260, 158 - 4, 110, 34);
    done.setBounds (getWidth() - 130, 158 - 4, 110, 34);
}

bool AssignEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------ FoldRail

FoldRail::FoldRail (const String& id, const String& title)
{
    setWantsKeyboardFocus (true);
    setTitle (title);
    setComponentID (id);
}

void FoldRail::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff141517));
    g.fillRect (r);
    g.setColour (juce::Colour (0xff2c2d31));
    g.fillRect (r.removeFromRight (2.0f));
    const auto c = Pt ((float) getWidth() / 2.0f - 1.0f, 34.0f);
    juce::Path tri;
    if (folded) tri.addTriangle (c.x - 8, c.y - 11, c.x - 8, c.y + 11, c.x + 11, c.y);
    else        tri.addTriangle (c.x - 11, c.y - 8, c.x + 11, c.y - 8, c.x, c.y + 11);
    g.setColour (pressed ? juce::Colour (0xff8a8c92) : hover ? juce::Colour (0xfff2f2ee) : juce::Colour (0xffc9cacd));
    g.fillPath (tri);
    if (focusFromKeyboard && hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (juce::Rectangle<float> (34.0f, 34.0f).withCentre (c), 5.0f, 2.0f);
    }
}

void FoldRail::mouseUp (const juce::MouseEvent& e)
{
    const auto inside = hitTest (0, juce::roundToInt (e.position.y)) && getLocalBounds().contains (e.position.toInt());
    pressed = false;
    repaint();
    if (inside && onToggle)
        onToggle();
}

bool FoldRail::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::spaceKey || key.getKeyCode() == juce::KeyPress::returnKey)
    {
        if (onToggle)
            onToggle();
        return true;
    }
    return false;
}

std::unique_ptr<juce::AccessibilityHandler> FoldRail::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button,
                                                         juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this] { if (onToggle) onToggle(); }));
}
} // namespace airack::studio::house
