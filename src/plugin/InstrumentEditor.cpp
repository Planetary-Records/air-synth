#include "InstrumentEditor.h"
#include "ReleaseInfo.h"
#include "RackExamples.h"
#include "SessionMarker.h"
#include "GenerationLog.h"
#include "EmptyRackSpace.h"
#include "HouseUnit.h"
#include "PatternUnit.h"
#include "AiUnit.h"
#include "LibraryUnit.h"
#include "ProgrammerUnit.h"
#include "RackPanel.h"
#include "RackStack.h"
#include "FxAnalysisClock.h"
#include "SessionUnit.h"
#include "PatternStrip.h"
#include "DrumSourcePopup.h"
#include <algorithm>

namespace airack
{
namespace
{
void identify(juce::Component& c,const juce::String& id,const juce::String& name)
{ c.setComponentID(id);c.setName(name); }
// The one-click way to a first sound on an empty instance. It stands inside the empty rack area and goes when a rack is there.
struct StarterButton final : juce::Button
{
    StarterButton():juce::Button("START WITH A SOUND")
    {
        identify(*this,"starter.sound","Start with a sound");
        setTitle("Start with a sound");setWantsKeyboardFocus(true);
        setTooltip("Places a ready-made rack so you can play straight away.");
    }
    void paintButton(juce::Graphics& g,bool over,bool down) override
    {
        const auto r=getLocalBounds().toFloat().reduced(1.5f);
        g.setColour(juce::Colour(down?0xff3a4048:over?0xff2f353c:0xff262b32));g.fillRoundedRectangle(r,r.getHeight()*.5f);
        g.setColour(juce::Colour(0xffe8b04a));g.drawRoundedRectangle(r,r.getHeight()*.5f,hasKeyboardFocus(true)?2.5f:1.2f);
        g.setFont(juce::FontOptions(14.0f,juce::Font::bold));g.setColour(juce::Colour(0xfff2e6c9));
        g.drawText(getButtonText(),getLocalBounds(),juce::Justification::centred,false);
    }
};
// The panel's connections in a real window: everything here reads this computer and shows the result to the musician.
FeedbackPanel::Host defaultFeedbackHost(juce::AudioProcessor& p)
{
    FeedbackPanel::Host h;
    h.environment=[&p]{
        feedback::Environment e;
        e.operatingSystem=juce::SystemStats::getOperatingSystemName();
        e.host=p.wrapperType==juce::AudioProcessor::wrapperType_Standalone?juce::String("Standalone"):juce::PluginHostType().getHostDescription();
        e.format=juce::AudioProcessor::getWrapperTypeDescription(p.wrapperType);
        e.sampleRate=p.getSampleRate();e.blockSize=p.getBlockSize();
        e.previousSessionEndedNormally=!feedback::SessionTracker::previousSessionEndedUnexpectedly();
        return e;
    };
    h.logLines=[]{
        // The newest diagnostic log, last lines only, with the person's name and computer name hidden.
        auto newest=juce::File();
        for(const auto& f:diagnostics::directory().findChildFiles(juce::File::findFiles,false,"*.log*"))
            if(newest==juce::File()||f.getLastModificationTime()>newest.getLastModificationTime())newest=f;
        if(newest==juce::File()||newest.getSize()>diagnostics::maximumLogBytes)return juce::StringArray();
        juce::StringArray words;
        for(const auto& w:{juce::SystemStats::getLogonName(),juce::SystemStats::getComputerName()})if(w.length()>=3)words.add(w);
        juce::StringArray lines;
        for(const auto& line:feedback::tailLines(newest.loadFileAsString(),feedback::maxLogLines,feedback::maxLogBytes))lines.add(feedback::scrub(line,words));
        return lines;
    };
    return h;
}
bool hasNoRacks(const SoundDocument& d)
{
    if(d.layout.synthCount!=0)return false;
    for(const auto count:d.layout.effectCount)if(count!=0)return false;
    return true;
}
}
// A small, always-visible reminder that this is early software. It never changes the units beneath it.
struct InstrumentEditor::AlphaBadge final : juce::Component, juce::SettableTooltipClient
{
    static constexpr int width=58,height=18,margin=6;
    std::function<void()> onClick;
    AlphaBadge()
    {
        identify(*this,"alpha.badge","Alpha version");
        setTitle(juce::String("Alpha version ")+airack::releaseVersion+". Opens the report a problem form.");
        setTooltip("air is an early test version ("+juce::String(airack::releaseVersion)+"). Save often and keep copies of your projects. Click to report a problem or share feedback.");
        // Deliberately not a tab stop: the studio's tab order is specified and tested; the badge is for the mouse.
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void mouseUp(const juce::MouseEvent& e) override{if(e.mouseWasClicked()&&onClick)onClick();}
    void paint(juce::Graphics& g) override
    {
        const auto r=getLocalBounds().toFloat().reduced(.5f);
        g.setColour(juce::Colour(0xe620252c));g.fillRoundedRectangle(r,height*.5f);
        g.setColour(juce::Colour(0xffe8b04a));g.drawRoundedRectangle(r,height*.5f,1.0f);
        g.setFont(juce::FontOptions(11.0f,juce::Font::bold));
        g.drawText("ALPHA",getLocalBounds(),juce::Justification::centred,false);
    }
};
// After a session that did not end normally: one quiet offer to prepare a report. Nothing is sent.
struct InstrumentEditor::NoticeBar final : juce::Component
{
    static constexpr int width=660,height=40;
    juce::Label text;
    juce::TextButton prepare{"Prepare a report"},dismiss{"Not now"};
    std::function<void()> onPrepare;
    NoticeBar()
    {
        identify(*this,"feedback.notice","Last session ended unexpectedly");
        text.setText("Last time, your music program closed unexpectedly. Want to tell us what happened?",juce::dontSendNotification);
        text.setFont(juce::FontOptions(13.0f));text.setColour(juce::Label::textColourId,juce::Colour(0xfff2e6c9));text.setInterceptsMouseClicks(false,false);
        identify(prepare,"feedback.notice.prepare","Prepare a report");identify(dismiss,"feedback.notice.dismiss","Not now");
        prepare.onClick=[this]{setVisible(false);if(onPrepare)onPrepare();};
        dismiss.onClick=[this]{setVisible(false);};
        addAndMakeVisible(text);addAndMakeVisible(prepare);addAndMakeVisible(dismiss);setVisible(false);
    }
    void paint(juce::Graphics& g) override
    {
        const auto r=getLocalBounds().toFloat().reduced(.5f);
        g.setColour(juce::Colour(0xf020252c));g.fillRoundedRectangle(r,8.0f);
        g.setColour(juce::Colour(0xffe8b04a));g.drawRoundedRectangle(r,8.0f,1.0f);
    }
    void resized() override
    {
        auto a=getLocalBounds().reduced(10,6);
        dismiss.setBounds(a.removeFromRight(80));a.removeFromRight(6);prepare.setBounds(a.removeFromRight(130));a.removeFromRight(8);
        text.setBounds(a);
    }
};
struct InstrumentEditor::Content final : juce::Component,private juce::Timer
{
    InstrumentProcessor& processor;
    const studio::Model& skin;
    // The stack's drawer.
    RackPanelComponent rackPanel;
    // The units and the rack stack sit on a stage of the skin's design pixels, scaled to fit; ADVANCED's PROGRAMMER
    // takes the rack area's place on it.
    juce::Component stage;
    studio::SessionUnit session;
    studio::PatternStrip strip;
    studio::PatternUnit patternUnit;
    studio::AiUnit ai;
    studio::LibraryUnit libraryUnit;
    std::uint64_t shownReset=0;
    bool first=true,closing=false,advanced=false,aiOpen=false,library=false,unfolded=false;
    bool libraryAdvanced=false,libraryAi=false,libraryUnfolded=false,libraryPlaced=false;
    juce::Point<int> libraryRackScroll;
    juce::Uuid libraryRackSelection=juce::Uuid::null();
    // Declared after the units, so they are destroyed while the rack panel, whose callback frame they hold, still lives.
    studio::HouseUnit house;
    studio::ProgrammerUnit programmer;
    studio::EmptyRackSpace space;
    StarterButton starter;
    // Declared after the house unit, the space and the drawer that it lays out, so it is destroyed first.
    studio::FxAnalysisClock analysis;
    RackStack stack;
    std::unique_ptr<studio::DrumSourcePopup> drumSource;
    Content(InstrumentProcessor&,const studio::Model&);
    ~Content() override { stopTimer(); }
    void closeForRetirement(){closing=true;stopTimer();stack.onDrumSource=nullptr;stack.onDrumSourceClose=nullptr;if(drumSource)drumSource->close();stack.retireGraphs();analysis.retire();ai.retire();patternUnit.retire();}
    // Active notification frames unwind first; detached UI can then wait for delivery or queue discard.
    static void detachParameterControls(juce::Component& component){
        if(auto* knob=dynamic_cast<studio::ProgrammerKnob*>(&component)){knob->retire();return;}
        if(auto* view=dynamic_cast<studio::house::View*>(&component)){view->retire();return;}
        for(auto* child:component.getChildren())detachParameterControls(*child);
    }
    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    void placeStarter();
    // One unit at a time: ADVANCED, the AI unit, the LIBRARY and the unfolded pattern unit never show together.
    void showUnits(bool toAdvanced,bool toAi,bool toLibrary,bool toUnfolded);
    // The house unit's and the PROGRAMMER's: the rack panel's callback frame, and edits through change().
    studio::house::Host unitHost()
    {
        const juce::Component::SafePointer<Content> owner(this);
        return {[owner]{return owner&&!owner->closing?owner->rackPanel.editorCallbackFrame():std::shared_ptr<void>{};},
                [owner]{return !owner||owner->closing;},
                [owner](const juce::String& text){if(owner&&!owner->closing)owner->error(text);},
                [owner](const std::function<void(SoundDocument&)>& edit,const juce::String& caption){if(owner&&!owner->closing)owner->change(edit,caption);}};
    }
    void result(bool ok,const juce::String& operation)
    { session.report(operation+(ok?" complete":" failed; sound preserved")); }
    void error(const juce::String& text) { session.report(text); }
    bool snapshot(SoundDocument& d,std::uint64_t& r)
    {
        std::uint64_t reset;
        if(processor.trySoundSnapshot(d,r,reset)) {
            if(!first&&reset!=shownReset){timerCallback();error("Sound recalled; pending selection cancelled. Select the edit again.");return false;}
            return true;
        }
        error("Sound is busy; retry the edit");return false;
    }
    template<class Change> void change(Change change,const juce::String& caption)
    {
        SoundDocument d;std::uint64_t r;
        if(snapshot(d,r)){change(d);result(processor.applySoundEdit(d,SoundEditOrigin::manual,r),caption);}
        timerCallback();
    }
};
InstrumentEditor::Content::Content(InstrumentProcessor& p,const studio::Model& m):processor(p),skin(m),rackPanel(p,skin),
    session(p,skin,studio::defaultStudioServices()),strip(p,skin,studio::defaultStudioServices()),
    patternUnit(p,skin,studio::defaultStudioServices()),ai(p,skin,studio::defaultStudioServices()),
    libraryUnit(p,skin,studio::defaultStudioServices(),{[this]{return rackPanel.editorCallbackFrame();},[this]{return closing;}}),
    house(p,skin,unitHost()),
    programmer(p,skin,{unitHost(),[this](SoundDocument& d,std::uint64_t& r){return snapshot(d,r);},[this]{timerCallback();}}),
    space(skin),analysis(p.fxMonitorHandle()),stack(p,skin,house,space,rackPanel,unitHost(),analysis.hub)
{
    identify(stage,"stage","Studio rack");stage.setInterceptsMouseClicks(false,true);
    stage.setSize((int)skin.layout["stage"][0],(int)skin.layout["stage"][1]);
    stage.addAndMakeVisible(session);stage.addAndMakeVisible(strip);strip.setTopLeftPosition(0,(int)skin.layout["strip"]["y"]);
    stage.addChildComponent(patternUnit);patternUnit.setTopLeftPosition(0,(int)skin.layout["pattern"]["y"]);
    // Each side unit's right is the stage's: the LIBRARY, a pixel narrower than the AI unit, stands a pixel right of side.x.
    for(auto* unit:std::initializer_list<juce::Component*>{&ai,&libraryUnit})
    {stage.addChildComponent(unit);unit->setTopLeftPosition(stage.getWidth()-unit->getWidth(),(int)skin.layout["side"]["y"]);unit->setExplicitFocusOrder(5);}
    stage.addAndMakeVisible(stack);
    stage.addChildComponent(programmer);programmer.setTopLeftPosition(0,(int)skin.layout["advanced"]["y"]);
    addAndMakeVisible(stage);
    stage.setExplicitFocusOrder(1);
    session.setExplicitFocusOrder(1);strip.setExplicitFocusOrder(2);patternUnit.setExplicitFocusOrder(2);stack.setExplicitFocusOrder(3);
    programmer.setExplicitFocusOrder(3);
    session.onEdit=[this]{timerCallback();};
    session.onLever=[this](bool toAdvanced){showUnits(toAdvanced,toAdvanced?false:aiOpen,toAdvanced?false:library,toAdvanced?false:unfolded);};
    session.onAi=[this](bool open){showUnits(open?false:advanced,open,open?false:library,open?false:unfolded);};
    strip.onFold=[this](bool unfold){showUnits(unfold?false:advanced,unfold?false:aiOpen,unfold?false:library,unfold);};
    patternUnit.onFold=strip.onFold;
    // One unit unfolded at a time: the rail, pressed over the unfolded pattern unit, swaps the two. In a narrow column it
    // opens and closes the house unit's 3U unit instead of folding it.
    house.onRail=[this]
    {
        if(unfolded){house.setFolded(false);showUnits(false,false,false,false);}
        else if(stack.narrow())stack.openHouse(!stack.houseOpen());
        else house.setFolded(!house.isFolded());
    };
    space.add.onClick=[this]{showUnits(false,false,true,false);};
    space.addChildComponent(starter);starter.onClick=[this]{placeStarter();};
    rackPanel.onAdvanced=[this]{showUnits(true,false,false,false);};
    stack.onSelectionChanged=[this](juce::Uuid id){libraryUnit.selectRack(id);};
    stack.onReport=[this](const juce::String& text){session.report(text);};
    stack.onEdited=[this]{timerCallback();};
    stack.onDrumSource=[this](DrumPadTarget target,juce::Component* from){
        const auto frame=rackPanel.editorCallbackFrame();if(closing)return;
        if(drumSource&&drumSource->isVisible()){session.report("Finish or cancel PAD "+juce::String(int(processor.drumImport().view().target.pad)+1).paddedLeft('0',2)+" source selection first");drumSource->toFront(true);return;}
        if(!drumSource){drumSource=std::make_unique<studio::DrumSourcePopup>(processor);addChildComponent(*drumSource);}
        const auto input=processor.drumImport().view();
        if(input.generation&&input.target==target&&input.phase!=DrumImportPhase::closed&&input.phase!=DrumImportPhase::kept){if(drumSource->openGeneration(target,from))resized();else session.report("Generator is busy or captured pad unavailable");}
        else if(drumSource->open(target,from)){resized();drumSource->chooseSource();}else session.report(processor.drumImport().view().message);
    };
    stack.onDrumSourceClose=[this](juce::Uuid rack){if(drumSource&&drumSource->isVisible()&&processor.drumImport().view().target.rack==rack)drumSource->close(processor.drumImport().view().phase!=DrumImportPhase::kept);};
    analysis.onUpdated=[this]{stack.refreshAnalysis();};
    // Closing, the LIBRARY widens the stack, so the selected rack, which its APPLY selects, is brought into view after.
    libraryUnit.onDone=[this]{
        if(libraryPlaced){showUnits(false,false,false,false);stack.reveal(stack.selected());}
        else{showUnits(libraryAdvanced,libraryAi,false,libraryUnfolded);stack.select(libraryRackSelection);stack.viewport().setViewPosition(libraryRackScroll);}
    };
    libraryUnit.onPlaced=[this](juce::Uuid id,const juce::String& outcome){libraryPlaced=true;timerCallback();stack.select(id);session.report(outcome);};
    showUnits(false,false,false,false);
    startTimer(20);timerCallback();
}
void InstrumentEditor::Content::placeStarter()
{
    SoundDocument d;std::uint64_t revision=0;
    if(!processor.trySoundSnapshot(d,revision)){session.report("Try again in a moment");return;}
    // Copper Arc is the first factory rack: the same placement the LIBRARY's ADD makes, in synth slot 1.
    RackOperation op;op.design=factoryRackDesigns()[0];op.newInstance=juce::Uuid();
    op.destination={RackRole::synth,masterChain,0};op.allocation={{0,0}};op.allowSlotReuse=true;
    if(processor.applyRackOperation(op,revision)==RackCandidateStatus::ready){timerCallback();stack.select(op.newInstance);session.report("Copper Arc is ready. Play a note.");}
    else session.report("Could not place a rack. Press + ADD RACK instead.");
}
void InstrumentEditor::Content::timerCallback()
{
    if(closing)return;
    session.refresh();strip.refresh();patternUnit.refresh();if(library)libraryUnit.refresh();
    SoundDocument d;std::uint64_t r,reset;if(!processor.trySoundSnapshot(d,r,reset))return;
    const bool recalled=first||reset!=shownReset;
    if(!first&&recalled&&drumSource)drumSource->close();
    house.refresh(d);programmer.refresh(d,recalled);stack.refresh(d);shownReset=reset;first=false;
    {
        // Only on an empty instance with the rack area in view, and only where it clears the + ADD RACK lettering.
        const auto in=space.add.getBounds();
        const bool wanted=hasNoRacks(d)&&!library&&!advanced&&!aiOpen&&in.getWidth()>=560&&in.getHeight()>=36;
        if(wanted)
        {
            // Under the hint, centred, where the eye lands after the lettering; beside the lettering on the low strip.
            const auto h=juce::jmin(38,in.getHeight()-12);const int w=250;
            if(space.stacked()&&space.hint.getBottom()+16+h<=in.getBottom())starter.setBounds(in.getCentreX()-w/2,space.hint.getBottom()+18,w,h);
            else starter.setBounds(in.getRight()-w-20,in.getCentreY()-h/2,w,h);
        }
        if(starter.isVisible()!=wanted)starter.setVisible(wanted);
    }
    stack.setPlacement(library?libraryUnit.placement():std::nullopt);
    const auto* proposal=processor.aiProposal();
    stack.setProposal(proposal!=nullptr&&proposal->candidate?&proposal->candidate->sound:nullptr);
}
void InstrumentEditor::Content::showUnits(bool toAdvanced,bool toAi,bool toLibrary,bool toUnfolded)
{
    if(toAdvanced&&drumSource)drumSource->close();
    if(toLibrary&&!library){libraryAdvanced=advanced;libraryAi=aiOpen;libraryUnfolded=unfolded;libraryPlaced=false;libraryRackScroll=stack.viewport().getViewPosition();libraryRackSelection=stack.selected();}
    advanced=toAdvanced;aiOpen=toAi;library=toLibrary;unfolded=toUnfolded;
    session.setLever(advanced);session.setAiOpen(aiOpen);strip.setUnfolded(unfolded);patternUnit.setUnfolded(unfolded);strip.setNarrow(aiOpen);
    programmer.setVisible(advanced);ai.setVisible(aiOpen);libraryUnit.setVisible(library);
    // The unfolded unit covers the strip's place at the stage's foot, and leaves the house unit room for its strip.
    patternUnit.setVisible(unfolded);strip.setVisible(!unfolded);
    stack.setVisible(!advanced&&!library);house.setStripOnly(unfolded);stack.setStripOnly(unfolded);
    space.add.setToggleState(library,juce::dontSendNotification);
    space.setHint(library?"Choose a rack in the LIBRARY, then ADD, or CANCEL."
                         :"Press + ADD RACK to place a rack, or AI to describe a sound.");
    resized();
}
void InstrumentEditor::Content::paint(juce::Graphics& g)
{
    const auto& gap=skin.layout["gap_bgr"];
    g.fillAll(juce::Colour((juce::uint8)(int)gap[2],(juce::uint8)(int)gap[1],(juce::uint8)(int)gap[0]));
}
void InstrumentEditor::Content::resized()
{
    // An empty editor would scale the stage to nothing, which no transform may do.
    if(getLocalBounds().isEmpty())return;
    const auto w=(float)stage.getWidth(),h=(float)stage.getHeight(),s=std::min((float)getWidth()/w,(float)getHeight()/h);
    stage.setTransform(juce::AffineTransform::scale(s).translated(((float)getWidth()-w*s)*.5f,((float)getHeight()-h*s)*.5f));
    // The stack fills the rack area down to the strip, or to the unfolded pattern unit; beside a side unit it spans the
    // narrowed strip's width, scaled down so that every rack shows whole and none scrolls sideways.
    const auto rackTop=(float)(int)skin.layout["advanced"]["y"],bottom=unfolded?(float)(int)skin.layout["pattern"]["y"]:(float)strip.getY();
    const auto column=aiOpen?(float)(int)skin.layout["strip_narrow"]["width"]:w;
    const auto k=column/w;
    stack.setBounds(0,0,juce::roundToInt(column/k),juce::roundToInt((bottom-rackTop)/k));
    stack.setTransform(juce::AffineTransform::scale(k).translated(0.0f,rackTop));
    libraryUnit.setBounds(0,juce::roundToInt(rackTop),juce::roundToInt(w),strip.getY()-juce::roundToInt(rackTop));
    if(drumSource){const auto width=std::min(720,getWidth()),height=std::min(450,getHeight());drumSource->setBounds((getWidth()-width)/2,(getHeight()-height)/2,width,height);drumSource->toFront(false);}
}
InstrumentEditor::InstrumentEditor(InstrumentProcessor& p):InstrumentEditor(p,studio::studioSkin()){}
InstrumentEditor::InstrumentEditor(InstrumentProcessor& p,const studio::Model* skin):AudioProcessorEditor(p)
{
    setName("air instrument editor");
    alphaBadge=std::make_unique<AlphaBadge>();
    notice=std::make_unique<NoticeBar>();
    feedbackPanel=std::make_unique<FeedbackPanel>(defaultFeedbackHost(p));
    alphaBadge->onClick=[this]{feedbackPanel->open(false);};
    notice->onPrepare=[this]{feedbackPanel->open(true);};
    if(skin!=nullptr){content=std::make_unique<Content>(p,*skin);addAndMakeVisible(*content);}
    else{identify(skinError,"skin.error","Studio skin error");skinError.setText("Studio skin failed to load: "+studio::studioSkinError(),juce::dontSendNotification);addAndMakeVisible(skinError);}
    // The badge shows at once; the notice and the report panel appear only when asked for.
    addAndMakeVisible(*alphaBadge);addChildComponent(*notice);addChildComponent(*feedbackPanel);
    setResizable(true,true);setResizeLimits(minimumWidth,minimumHeight,2200,1600);setSize(minimumWidth,minimumHeight);
    if(feedback::SessionTracker::takeUnexpectedEndNotice())showUnexpectedEndNotice();
}
InstrumentEditor::~InstrumentEditor()
{
    if(!content)return;
    auto retained=std::shared_ptr<Content>(content.release());retained->closeForRetirement();
    const auto cleanup=[retained]{
        Content::detachParameterControls(*retained);
        retained->stack.deferEditorDestruction(retained);
    };
    if(!retained->rackPanel.deferEditorDestruction(retained,cleanup))cleanup();
    removeChildComponent(retained.get());
}
void InstrumentEditor::paint(juce::Graphics& g){g.fillAll(juce::Colour(0xff20252c));}
void InstrumentEditor::resized()
{
    if(content)content->setBounds(getLocalBounds());
    skinError.setBounds(getLocalBounds().reduced(14));
    alphaBadge->setBounds(getWidth()-AlphaBadge::width-AlphaBadge::margin,AlphaBadge::margin,AlphaBadge::width,AlphaBadge::height);
    alphaBadge->toFront(false);
    const auto noticeWidth=juce::jmin(NoticeBar::width,getWidth()-20);
    notice->setBounds((getWidth()-noticeWidth)/2,84,noticeWidth,NoticeBar::height);notice->toFront(false);
    feedbackPanel->setBounds(getLocalBounds());feedbackPanel->toFront(false);
}
void InstrumentEditor::showUnexpectedEndNotice(){notice->setVisible(true);resized();}
void InstrumentEditor::parentHierarchyChanged()
{
    if(getAudioProcessor()->wrapperType==juce::AudioProcessor::wrapperType_Standalone)
        if(auto* window=findParentComponentOfClass<juce::DocumentWindow>())
            window->setName(juce::String("air ")+airack::releaseVersion);
}
}
