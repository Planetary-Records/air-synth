#pragma once

#include "ScreenControls.h"
#include "StudioServices.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <functional>
#include <vector>

// Presses on a side unit's screen as a user makes them, for the units' tests: a click or right-click on an item,
// after the wheel has scrolled its list until it is on the glass, and an entry's text written and committed.

// The chooser records each request and answers at once with `answer`, or holds the callback; the clock is set by hand.
// A file drag is recorded and starts unless `dragStarts` is false; `endDrag` drops it, as the system's drag ends.
struct FakeServices
{
    struct Drag { juce::StringArray files; bool canMove=false; juce::Component* source=nullptr; };
    std::int64_t now=1000000;
    std::vector<airack::studio::FileRequest> requests;
    bool hold=false;
    juce::File answer;
    std::function<void(const juce::File&)> held;
    std::vector<Drag> drags;
    bool dragStarts=true;
    std::function<void()> dragDone;
    juce::String copied;
    airack::studio::StudioServices make()
    {
        airack::studio::StudioServices s;
        s.nowMs=[this]{return now;};
        s.copyText=[this](const juce::String& text){copied=text;};
        s.chooseFile=[this](const airack::studio::FileRequest& r,std::function<void(const juce::File&)> done)
        { requests.push_back(r);if(hold)held=std::move(done);else done(answer); };
        s.dragFiles=[this](const juce::StringArray& files,bool canMove,juce::Component* source,std::function<void()> done)
        { drags.push_back({files,canMove,source});if(!dragStarts)return false;dragDone=std::move(done);return true; };
        return s;
    }
    void endDrag() { if(auto done=std::move(dragDone)){dragDone={};done();} }
};

namespace screendriver
{
using Hit=airack::studio::Screen::Hit;
inline juce::MouseEvent event(juce::Component& c,juce::Point<float> at,juce::ModifierKeys mods)
{
    const auto t=juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),at,mods,juce::MouseInputSource::defaultPressure,
        juce::MouseInputSource::defaultOrientation,juce::MouseInputSource::defaultRotation,juce::MouseInputSource::defaultTiltX,
        juce::MouseInputSource::defaultTiltY,&c,&c,t,at,t,1,false);
}
// One step of the wheel over the glass: down the list for a negative delta.
inline void wheel(airack::studio::Screen& s,float deltaY)
{
    juce::MouseWheelDetails w;
    w.deltaX=0.0f;w.deltaY=deltaY;w.isReversed=w.isSmooth=w.isInertial=false;
    s.mouseWheelMove(event(s,s.getLocalBounds().getCentre().toFloat(),{}),w);
}
// The first hit `wanted` takes, scrolling the page's list from its top a row at a time until one is on the glass.
inline const Hit* reveal(airack::studio::Screen& s,const std::function<bool(const Hit&)>& wanted)
{
    const auto found=[&]()->const Hit* { for(auto& h:s.hits)if(wanted(h))return &h; return nullptr; };
    if(auto* h=found())return h;
    for(int i=0;i<64&&s.scrollOf(s.page)>0;++i)wheel(s,1.0f);
    for(int i=0;i<64;++i)
    {
        if(auto* h=found())return h;
        const auto before=s.scrollOf(s.page);
        wheel(s,-1.0f);
        if(s.scrollOf(s.page)==before)break;
    }
    return found();
}
} // namespace screendriver

// The screen inside a unit by its component ID.
inline airack::studio::Screen* screenOf(juce::Component& root,const char* id)
{
    if(auto* s=dynamic_cast<airack::studio::Screen*>(&root);s!=nullptr&&root.getComponentID()==id)return s;
    for(auto* child:root.getChildren())if(auto* s=screenOf(*child,id))return s;
    return nullptr;
}
// A click, or a right-click for `back`, at the centre of a hit of the page as it is now.
inline bool clickHit(airack::studio::Screen& s,const screendriver::Hit* h,bool back=false)
{
    if(h==nullptr)return false;
    const auto at=h->r.getCentre();
    s.mouseDown(screendriver::event(s,at,back?juce::ModifierKeys::rightButtonModifier:juce::ModifierKeys::leftButtonModifier));
    s.mouseUp(screendriver::event(s,at,{}));
    return true;
}
// Presses the item taking `command` (or opening its entry); `value` picks a list's row.
inline bool pressItem(airack::studio::Screen& s,const juce::String& command,int value=-1,bool back=false)
{
    return clickHit(s,screendriver::reveal(s,[&](const screendriver::Hit& h)
        { return (h.command==command||h.entry==command)&&(value<0||h.value==value||h.row==value); }),back);
}
// Presses an option's row until the unit shows it at `index`.
inline bool stepTo(airack::studio::Screen& s,const juce::String& command,int index)
{
    for(int i=0;i<16;++i)
    {
        if(const auto at=s.shown.options.find(command);at!=s.shown.options.end()&&at->second==index)return true;
        if(!pressItem(s,command))return false;
    }
    return false;
}
// Opens the item's entry, writes `text` over what it opened with and commits it as Return does a line, or as the
// prompt's focus leaving does. Their handlers run directly: a test window's focus can arrive late.
inline bool enterText(airack::studio::Screen& s,const juce::String& command,const juce::String& text)
{
    if(!pressItem(s,command)||!s.entry.isVisible()||s.entry.getComponentID()!=command)return false;
    s.entry.setText(text,false);
    if(s.entry.isMultiLine()){if(s.entry.onFocusLost)s.entry.onFocusLost();}
    else if(s.entry.onReturnKey)s.entry.onReturnKey();
    return !s.entry.isVisible();
}
// The row taking `command` whose tooltip contains `needle`, scrolled onto the glass.
inline const screendriver::Hit* hitWithTip(airack::studio::Screen& s,const juce::String& command,const juce::String& needle)
{
    return screendriver::reveal(s,[&](const screendriver::Hit& h){ return h.command==command&&h.tip.contains(needle); });
}
