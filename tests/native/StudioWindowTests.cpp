#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "PreparedMakerSkins.h"
#include "StudioSkin.h"
#include "SkinView.h"
#include "ScreenControls.h"
#include "ScreenDriver.h"
#include "SessionUnit.h"
#include "FactorySounds.h"
#include "PatternStrip.h"
#include "PatternControls.h"
#include "PatternUnit.h"
#include "HouseUnit.h"
#include "LibraryUnit.h"
#include "DrumSourcePopup.h"
#include "ProgrammerLook.h"
#include "ProgrammerUnit.h"
#include "EmptyRackSpace.h"
#include "RackExamples.h"
#include "WheelLatch.h"
#include "RackLibrary.h"
#include "PatternIO.h"
#include "InstrumentProcessor.h"
#include "InstrumentEditor.h"
#include "ReleaseInfo.h"
#include "FeedbackPanel.h"
#include "SessionMarker.h"
#include "RackPanel.h"
#include "RackReview.h"
#include "RackStack.h"
#include "SaturationGraph.h"
#include "SpaceReverbGraph.h"
#include "UserReach.h"
#include "Motion.h"
#include "PostedClick.h"
#include "BlankInstance.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <mutex>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace airack {void setRackLibrarySkinTestHook(std::function<void(std::vector<RackInstance>,std::optional<RackInstance>,std::vector<RackInstance>,bool,bool)>);}
namespace
{
using namespace airack;
int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cout << "FAIL " << __LINE__ << ": " << #x << '\n'; } } while(false)
juce::File output;
void pump(int ms=35) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
template<class F> bool wait(const char* name,F ready)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!ready() && std::chrono::steady_clock::now()<end) pump(1);
    if(ready()) return true;
    std::cout << "UI deadline 3000ms: " << name << '\n'; return false;
}
juce::Component* find(juce::Component& root,const juce::String& id)
{
    if(root.getComponentID()==id) return &root;
    for(auto* child:root.getChildren()) if(auto* found=find(*child,id)) return found;
    return nullptr;
}
template<class T> T* widget(juce::Component& root,const char* id)
{ auto* result=dynamic_cast<T*>(find(root,id)); CHECK(result!=nullptr); return result; }
void click(juce::Button& button,int ms=35) { CHECK(clickPosted(button)); pump(ms); }
void click(juce::Component& root,const char* id) { if(auto* c=widget<juce::Button>(root,id)) click(*c); }
using studio::Pt; using studio::Rect;
// The prototype's self-test probes (window/skin/app/Main.cpp), in view coordinates.
juce::Image snap(juce::Component& c) { return c.createComponentSnapshot(c.getLocalBounds(),true,1.0f); }
bool writePng(const juce::File& file,const juce::Image& image)
{
    file.deleteFile();
    juce::FileOutputStream out(file);
    return out.openedOk()&&juce::PNGImageFormat().writeImageToStream(image,out);
}
double luma(juce::Colour c) { return 255.0*(0.114*c.getFloatBlue()+0.587*c.getFloatGreen()+0.299*c.getFloatRed()); }
double meanLuma(const juce::Image& img,Rect r)
{
    const auto b=r.getSmallestIntegerContainer().getIntersection(img.getBounds());
    const juce::Image::BitmapData bd(img,juce::Image::BitmapData::readOnly);
    double sum=0;
    for(int y=b.getY();y<b.getBottom();++y) for(int x=b.getX();x<b.getRight();++x) sum+=luma(bd.getPixelColour(x,y));
    return sum/std::max(1,b.getWidth()*b.getHeight());
}
// The ink above a luma threshold inside a box, as pixel edges (right and bottom exclusive).
Rect inkBox(const juce::Image& img,Rect box,double threshold)
{
    const auto b=box.getSmallestIntegerContainer().getIntersection(img.getBounds());
    const juce::Image::BitmapData bd(img,juce::Image::BitmapData::readOnly);
    int l=1<<20,t=1<<20,r=-1,btm=-1;
    for(int y=b.getY();y<b.getBottom();++y) for(int x=b.getX();x<b.getRight();++x)
        if(luma(bd.getPixelColour(x,y))>threshold){l=std::min(l,x);t=std::min(t,y);r=std::max(r,x);btm=std::max(btm,y);}
    return r<0?Rect():Rect::leftTopRightBottom((float)l,(float)t,(float)r+1,(float)btm+1);
}
Pt local(juce::Component& c,Pt viewPoint) { return viewPoint-c.getPosition().toFloat(); }
juce::MouseEvent event(juce::Component& c,Pt at,juce::ModifierKeys mods,int clicks,bool dragged,std::optional<Pt> down={})
{
    const auto t=juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),at,mods,juce::MouseInputSource::defaultPressure,
        juce::MouseInputSource::defaultOrientation,juce::MouseInputSource::defaultRotation,juce::MouseInputSource::defaultTiltX,
        juce::MouseInputSource::defaultTiltY,&c,&c,t,down.value_or(at),t,clicks,dragged);
}
// A press, ten moves and a release on the control's own mouse handlers.
void drag(juce::Component& c,Pt fromView,Pt toView,juce::ModifierKeys extra={})
{
    const auto from=local(c,fromView),to=local(c,toView);
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier|extra.getRawFlags());
    c.mouseDown(event(c,from,held,1,false));
    for(int i=1;i<=10;++i) c.mouseDrag(event(c,from+(to-from)*((float)i/10.0f),held,1,true,from));
    c.mouseUp(event(c,to,{},1,true,from));
}
void clickAt(juce::Component& c,Pt viewPoint)
{
    const auto at=local(c,viewPoint);
    c.mouseDown(event(c,at,juce::ModifierKeys::leftButtonModifier,1,false));
    c.mouseUp(event(c,at,{},1,false));
}
void press(studio::SkinButton& b) { clickAt(b,b.element.rect.getCentre()); }
juce::MouseWheelDetails wheel(float deltaY)
{
    juce::MouseWheelDetails w;
    w.deltaX=0.0f;w.deltaY=deltaY;w.isReversed=w.isSmooth=w.isInertial=false;
    return w;
}
// The session bar and the folded strip on the stage, on the desktop so that focus works, counting each control's commands.
struct Rig final:juce::Component
{
    const studio::Model& model=*studio::studioSkin();
    studio::SkinView session{model,"session"},strip{model,"strip"};
    std::map<juce::String,int> commands;
    Rig()
    {
        addAndMakeVisible(session);addAndMakeVisible(strip);
        strip.setTopLeftPosition(0,(int)model.layout["strip"]["y"]);
        setSize(1547,965);
        for(auto* v:{&session,&strip})
        {
            for(auto& b:v->buttons) b->onClick=[this,id=b->element.id]{++commands[id];};
            for(auto& l:v->levers) l->onClick=[this,id=l->element.id]{++commands[id];};
        }
        strip.fold->onPress=[this]{++commands["strip.fold"];};
        addToDesktop(0);setVisible(true);
    }
    ~Rig() override { removeFromDesktop(); }
};
studio::SessionShown drawnSession(const studio::Model& m)
{
    studio::SessionShown s;
    s.layout="name";s.name="WARM CIRCUIT";s.tempoKey="host";s.tempoText="120 BPM HOST";s.path=m.mockup.path;
    s.accessible=m.mockup.path+". Host tempo: 120 BPM";
    return s;
}
studio::StripShown drawnStrip()
{
    studio::StripShown s;
    s.layout="settings";s.text="4 BARS  4/4  1/16  SWING 0%";s.accessible="Pattern settings";
    return s;
}
std::array<double,3> meanChannels(const juce::Image& img,Rect r)
{
    const auto b=r.getSmallestIntegerContainer().getIntersection(img.getBounds());
    const juce::Image::BitmapData bd(img,juce::Image::BitmapData::readOnly);
    std::array<double,3> sum{};
    for(int y=b.getY();y<b.getBottom();++y) for(int x=b.getX();x<b.getRight();++x)
    { const auto c=bd.getPixelColour(x,y);sum[0]+=c.getRed();sum[1]+=c.getGreen();sum[2]+=c.getBlue(); }
    for(auto& s:sum)s/=std::max(1,b.getWidth()*b.getHeight());
    return sum;
}
// InstrumentTests.cpp's processor fixtures.
struct Playhead:juce::AudioPlayHead
{
    bool playing=false,available=true;std::optional<double> bpm,ppq;
    juce::Optional<PositionInfo> getPosition() const override
    { if(!available)return {};PositionInfo p;p.setIsPlaying(playing);if(bpm)p.setBpm(*bpm);if(ppq)p.setPpqPosition(*ppq);return p; }
};
std::size_t idx(const juce::String& id)
{
    const auto r=parameterRegistry();
    for(std::size_t i=0;i<r.size();++i)if(r[i].id==id)return i;
    CHECK(false);return 0;
}
void physical(SoundDocument& d,const juce::String& id,float value)
{ const auto i=idx(id);const auto& c=parameterRegistry()[i];d.values[i]=juce::NormalisableRange<float>(c.minimum,c.maximum,0,c.skew).convertTo0to1(value); }
SoundDocument document(InstrumentProcessor& p) { SoundDocument d;std::uint64_t r=0;CHECK(p.trySoundSnapshot(d,r));return d; }
bool edit(InstrumentProcessor& p,const SoundDocument& d) { SoundDocument now;std::uint64_t r=0;CHECK(p.trySoundSnapshot(now,r));return p.applySoundEdit(d,SoundEditOrigin::manual,r); }
juce::MemoryBlock state(const SoundDocument& d) { juce::MemoryBlock b;CHECK(encodeSoundState(d,b));return b; }
bool restore(InstrumentProcessor& p,const juce::MemoryBlock& b) { return p.restoreSoundState(b.getData(),static_cast<int>(b.getSize())); }
const juce::String loadErrorSentence="Could not load saved sound: invalid or unsupported data. Current sound preserved.";
// A SESSION unit on the desktop over a prepared processor, its preset path <temp>/WARM CIRCUIT.airack and, unless
// told otherwise, a 120 BPM host.
struct SessionRig
{
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    FakeServices services;
    Playhead head;
    const juce::File dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("studio-session-"+juce::Uuid().toString());
    const juce::String warm=dir.getChildFile("WARM CIRCUIT.airack").getFullPathName();
    std::unique_ptr<studio::SessionUnit> unit;
    explicit SessionRig(std::optional<double> bpm=120.0)
    {
        CHECK(blankInstance(p));
        dir.createDirectory();
        if(bpm){head.bpm=bpm;p.setPlayHead(&head);}
        p.prepareToPlay(48000,64);block();
        unit=std::make_unique<studio::SessionUnit>(p,*studio::studioSkin(),services.make());
        unit->addToDesktop(0);unit->setVisible(true);
        path(warm);
    }
    ~SessionRig() { unit.reset();p.setPlayHead(nullptr);p.releaseResources();dir.deleteRecursively(); }
    const studio::Model& model() const { return *studio::studioSkin(); }
    void block(bool withNote=false)
    {
        juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;
        if(withNote)midi.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),0);
        p.processBlock(audio,midi);
    }
    void path(const juce::String& text) { if(auto* e=widget<juce::TextEditor>(*unit,"preset.path")){e->setText(text);pump();} }
    studio::Display& display() { return *widget<studio::Display>(*unit,"session.display"); }
    studio::SessionShown shown() { unit->refresh();return display().session; }
    bool errorLit() { auto* e=widget<studio::Indicator>(*unit,"session.error");return e!=nullptr&&e->isLit(); }
    juce::String label(const char* id) { auto* l=widget<juce::Label>(*unit,id);return l!=nullptr?l->getText():juce::String("<missing>"); }
    int hold(const juce::String& today) const
    {
        for(const auto& m:model().displays.at("session").messages)if(m.today==today)return m.holdMs;
        CHECK(false);return 0;
    }
    // A path SAVE cannot write: its folder is a file.
    juce::String blocked() const
    {
        const auto file=dir.getChildFile("not a folder");
        file.replaceWithText("x");
        return file.getChildFile("WARM CIRCUIT.airack").getFullPathName();
    }
};
// A PATTERN strip on the desktop over a processor, prepared unless told otherwise.
struct StripRig
{
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    FakeServices services;
    Playhead head;
    const juce::File dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("studio-strip-"+juce::Uuid().toString());
    // The strip sits in a parent, as in the editor: a desktop window's own size is rounded to the display's scale.
    juce::Component holder;
    std::unique_ptr<studio::PatternStrip> strip;
    explicit StripRig(bool prepared=true)
    {
        dir.createDirectory();
        if(prepared)p.prepareToPlay(48000,64);
        strip=std::make_unique<studio::PatternStrip>(p,*studio::studioSkin(),services.make());
        holder.setSize(strip->getWidth(),strip->getHeight());holder.addAndMakeVisible(*strip);
        holder.addToDesktop(0);holder.setVisible(true);
    }
    ~StripRig() { strip.reset();p.setPlayHead(nullptr);p.releaseResources();dir.deleteRecursively(); }
    const studio::Model& model() const { return *studio::studioSkin(); }
    void block() { juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;p.processBlock(audio,midi); }
    ProjectDocument project(std::uint64_t* revision=nullptr)
    { ProjectDocument d;std::uint64_t e=0,r=0;CHECK(p.tryProjectSnapshot(d,e,r));if(revision)*revision=e;return d; }
    void pattern(const PatternDocument& d) { std::uint64_t e=0;project(&e);CHECK(p.applyPatternEdit(d,e));strip->refresh(); }
    void mode(PlaybackMode m) { std::uint64_t e=0;project(&e);CHECK(p.setPlaybackMode(m,e));strip->refresh(); }
    studio::Display& display() { return *widget<studio::Display>(*strip,"strip.display"); }
    studio::StripShown shown() { strip->refresh();return display().strip; }
    juce::String status() { auto* l=widget<juce::Label>(*strip,"strip.status");return l!=nullptr?l->getText():juce::String("<missing>"); }
};
// The editor on the desktop, over a processor that never launches the companion. Its pipe is its own: on the default
// pipe the editor's account status would reach a companion the user left running, which logs to the user's folder. A
// test pipe needs a fixture root, which only a launch would use.
struct WindowRig
{
    static companion::ClientOptions offline()
    {
        companion::ClientOptions o;o.allowLaunch=false;
        o.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");
        o.launch.fixtureRoot=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("studio-window-tests");
        return o;
    }
    InstrumentProcessor p{offline(), airack::testing::isolatedRackRuntime()};
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    WindowRig() { CHECK(blankInstance(p));editor.reset(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump(); }
    ~WindowRig() { editor.reset();pump(); }
    const studio::Model& model() const { return *studio::studioSkin(); }
    juce::Component* stage() { return find(*editor,"stage"); }
    bool showing(const char* id) { auto* c=find(*editor,id);return c!=nullptr&&c->isShowing(); }
    studio::HouseUnit* house() { return dynamic_cast<studio::HouseUnit*>(find(*editor,"house.panel")); }
    // The rack area's stack, around its viewport rack.stack, and its rack units from the top.
    RackStack* stack() { auto* view=find(*editor,"rack.stack");return view!=nullptr?dynamic_cast<RackStack*>(view->getParentComponent()):nullptr; }
    RackStackUnit* rack(int n) { return dynamic_cast<RackStackUnit*>(find(*editor,"rack.unit."+juce::String(n))); }
    // The stack body's own rack panel, the drawer.
    RackPanelComponent* drawer()
    {
        auto* s=stack();
        if(s!=nullptr)for(auto* c:s->body().getChildren())if(auto* panel=dynamic_cast<RackPanelComponent*>(c))return panel;
        return nullptr;
    }
    bool inLibrary() { return showing("library.panel"); }
    // A component's rectangle in the editor's coordinates.
    Rect box(juce::Component& c) { return editor->getLocalArea(&c,c.getLocalBounds().toFloat()); }
    std::array<std::uint64_t,4> revisions()
    {
        SoundDocument s;ProjectDocument d;std::uint64_t se=0,sr=0,pe=0,pr=0;
        CHECK(p.trySoundSnapshot(s,se,sr)&&p.tryProjectSnapshot(d,pe,pr));
        return {se,sr,pe,pr};
    }
};

bool chooseLibrary(juce::Component& root,const juce::String& name)
{
    auto* library=dynamic_cast<studio::LibraryUnit*>(find(root,"library.panel"));
    if(!library)return false;
    if(library->browserState().detailOpen)click(root,"library.back");
    auto* search=dynamic_cast<juce::TextEditor*>(find(root,"library.search"));
    auto* gallery=dynamic_cast<juce::ListBox*>(find(root,"library.gallery"));
    if(!search||!gallery)return false;
    search->setText(name,true);
    if(!wait("Gallery query",[&]{library->refresh();return library->browserState().query.text==name&&library->matchingRows()>0;}))return false;
    gallery->selectRow(0);gallery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    return wait("exact local Add preparation",[&]{library->refresh();auto* add=dynamic_cast<juce::Button*>(find(root,"library.apply"));return add&&add->isEnabled();});
}

juce::Array<int> itemIds(juce::ComboBox& box) { juce::Array<int> ids;for(int i=0;i<box.getNumItems();++i)ids.add(box.getItemId(i));return ids; }
bool closeTo(Rect got,Rect want) { return std::abs(got.getX()-want.getX())<=1.0f&&std::abs(got.getY()-want.getY())<=1.0f&&std::abs(got.getRight()-want.getRight())<=1.0f&&std::abs(got.getBottom()-want.getBottom())<=1.0f; }
const juce::String hintLine="Press + ADD RACK to place a rack, or AI to describe a sound.",
                   libraryHint="Choose a rack in the LIBRARY, then ADD, or CANCEL.";
double peakLuma(const juce::Image& img,Rect r)
{
    const auto b=r.getSmallestIntegerContainer().getIntersection(img.getBounds());
    const juce::Image::BitmapData bd(img,juce::Image::BitmapData::readOnly);
    double hi=0;
    for(int y=b.getY();y<b.getBottom();++y) for(int x=b.getX();x<b.getRight();++x) hi=std::max(hi,luma(bd.getPixelColour(x,y)));
    return hi;
}
PatternDocument settings(std::uint8_t bars,PhraseMeter meter,PatternGrid grid,double swing)
{ PatternDocument d;d.bars=bars;d.meter=meter;d.grid=grid;d.swing=swing;return d; }
// The drawn pattern (elements.json's mockup) as a processor pattern.
PatternDocument mockupPattern(const studio::Model& m)
{
    auto d=settings((std::uint8_t)m.mockup.bars,{(std::uint8_t)m.mockup.numerator,(std::uint8_t)m.mockup.denominator},PatternGrid::sixteenth,m.mockup.swing);
    d.noteCount=(std::uint16_t)m.mockup.notes.size();
    for(std::size_t i=0;i<m.mockup.notes.size();++i)
    {
        const auto& n=m.mockup.notes[i];
        d.notes[i]={n.start,n.duration,(std::uint8_t)n.pitch,(std::uint8_t)n.velocity,(std::uint8_t)n.channel};
    }
    return d;
}

void skinLoadsFromTheBinary()
{
    std::cout<<"SECTION skin_loads_from_the_binary\n";
    const auto* m=studio::studioSkin();
    CHECK(m!=nullptr);
    if(m==nullptr){std::cout<<studio::studioSkinError()<<'\n';return;}
    std::map<juce::String,int> perView;
    int missingLayers=0;
    for(const auto& e:m->elements)
    {
        ++perView[e.view];
        for(const auto& [role,key]:e.layers) if(!key.startsWith("procedural:")&&key!="plate"&&!m->assets.count(key)) ++missingLayers;
    }
    CHECK(perView["session"]==9&&perView["strip"]==5&&perView["ai"]==10&&perView["library"]==3&&perView["pattern"]==25);
    CHECK(missingLayers==0);
    const std::pair<const char*,juce::Point<int>> plates[]{{"session",{1547,107}},{"strip",{1547,104}},
        {"pattern",{1547,702}},{"advanced",{1547,754}},{"ai",{401,852}},{"library",{400,854}}};
    for(const auto& [view,size]:plates)
    {
        const auto it=m->plates.find(view);
        CHECK(it!=m->plates.end()&&it->second.getWidth()==size.x&&it->second.getHeight()==size.y);
    }
    CHECK(m->face!=nullptr&&m->face->getName()=="Inconsolata");
    CHECK(m->screenFace!=nullptr&&m->screenFace->getName()=="IBM Plex Sans Condensed");
    CHECK(m->markerFace!=nullptr&&m->markerFace->getName()=="Permanent Marker");
    CHECK((int)m->layout["stage"][0]==1547&&(int)m->layout["stage"][1]==965);
    const auto source=juce::JSON::parse(juce::File(RACKSYNTH_WORKSPACE).getChildFile("resources/studio-skin/SOURCE.json"));
    int files=0;
    if(auto* list=source["files"].getArray())
        for(const auto& f:*list)
        {
            ++files;
            const auto bytes=studio::studioSkinFile(f["path"].toString());
            CHECK(bytes.getSize()>0&&juce::SHA256(bytes).toHexString()==f["sha256"].toString());
        }
    CHECK(files==45);
    CHECK(studio::studioSkinFile("plates/missing.png").getSize()==0);
}
void hitRegions()
{
    std::cout<<"SECTION hit_regions\n";
    Rig rig;
    int ok=0,total=0;
    juce::StringArray misses;
    auto probe=[&](bool passed,const juce::String& what){++total;ok+=passed;if(!passed)misses.add(what);};
    for(auto* v:{&rig.session,&rig.strip})
    {
        for(auto& b:v->buttons)
        {
            probe(v->getComponentAt(b->element.rect.getCentre().toInt())==b.get(),b->element.id+" centre");
            probe(v->getComponentAt(Pt(b->element.rect.getX()-7.0f,b->element.rect.getCentreY()).toInt())!=b.get(),b->element.id+" outside");
        }
        for(auto& l:v->levers)
        {
            probe(v->getComponentAt(l->element.pivot.toInt())==l.get(),l->element.id+" pivot");
            probe(v->getComponentAt(l->element.footprint.getCentre().toInt())==l.get(),l->element.id+" lever");
            probe(v->getComponentAt(Pt(l->element.hitRect.getX()-4.0f,l->element.pivot.y).toInt())!=l.get(),l->element.id+" outside");
        }
        if(v->display) probe(v->getComponentAt(v->display->element.glass.getCentre().toInt())==v->display.get(),v->display->element.id+" glass");
    }
    probe(rig.strip.getComponentAt(rig.strip.fold->element.triangle.getCentre().toInt())==rig.strip.fold.get(),"strip.fold triangle");
    probe(rig.strip.getComponentAt(rig.strip.overview->element.glass.getCentre().toInt())==rig.strip.overview.get(),"strip.overview glass");
    probe(rig.session.getComponentAt(rig.session.error->element.led.toInt())!=rig.session.error.get(),"session.error passes clicks");
    CHECK(ok==total&&total==25);
    if(!misses.isEmpty())std::cout<<"missed: "<<misses.joinIntoString(", ")<<'\n';
}
void pressIsOneCommand()
{
    std::cout<<"SECTION press_is_one_command\n";
    Rig rig;
    juce::StringArray wrong;
    for(auto* v:{&rig.session,&rig.strip})
        for(auto& b:v->buttons)
        {
            const auto before=rig.commands[b->element.id];
            press(*b);
            if(rig.commands[b->element.id]!=before+1)wrong.add(b->element.id);
        }
    CHECK(wrong.isEmpty());
    auto& save=*rig.session.get<studio::SkinButton>("session.save");
    const auto before=rig.commands["session.save"];
    const auto at=local(save,save.element.rect.getCentre());
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    save.mouseDown(event(save,at,held,1,false));
    save.mouseDrag(event(save,at+Pt(60.0f,0.0f),held,1,true,at));
    save.mouseUp(event(save,at+Pt(60.0f,0.0f),{},1,true,at));
    CHECK(rig.commands["session.save"]==before);
    save.setEnabled(false);
    press(save);
    click(save);
    CHECK(rig.commands["session.save"]==before);
    save.setEnabled(true);
    click(save);
    CHECK(rig.commands["session.save"]==before+1);
    auto& undo=*rig.session.get<studio::SkinButton>("session.undo");
    const auto undone=rig.commands["session.undo"];
    undo.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    undo.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(rig.commands["session.undo"]==undone+2);
    rig.strip.fold->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    clickAt(*rig.strip.fold,rig.strip.fold->element.triangle.getCentre());
    CHECK(rig.commands["strip.fold"]==2);
}
void leverRules()
{
    std::cout<<"SECTION lever_rules\n";
    Rig rig;
    auto& rack=*rig.session.get<studio::Lever>("session.rack");
    auto& mode=*rig.strip.get<studio::Lever>("strip.mode");
    auto& n=rig.commands["session.rack"];
    const auto tip=Rect(rack.element.footprint.getWidth(),14.0f).withCentre({rack.element.pivot.x,rack.element.pivot.y+18.0f});
    const auto up=meanLuma(snap(rig.session),tip);
    clickAt(rack,rack.element.pivot);
    const auto down=meanLuma(snap(rig.session),tip);
    CHECK(rack.position()==1&&rack.getToggleState()&&n==1);
    CHECK(std::abs(down-up)>12.0);
    CHECK(std::abs(up-52.1)<0.5&&std::abs(down-69.1)<0.5);
    drag(rack,rack.element.pivot,rack.element.pivot-Pt(0.0f,14.0f));
    CHECK(rack.position()==0&&n==2);
    drag(rack,rack.element.pivot,rack.element.pivot-Pt(0.0f,14.0f));
    CHECK(rack.position()==0&&n==2);
    drag(rack,rack.element.pivot,rack.element.pivot+Pt(0.0f,4.0f));
    CHECK(rack.position()==1&&n==3);
    rack.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    rack.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(rack.position()==0&&n==4);
    rack.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    CHECK(rack.position()==1&&n==5);
    click(rack);
    CHECK(rack.position()==0&&n==6);
    rack.setPosition(1,juce::dontSendNotification);
    CHECK(rack.position()==1&&n==6);
    drag(mode,mode.element.pivot,mode.element.pivot+Pt(0.0f,14.0f));
    CHECK(mode.position()==1&&rig.commands["strip.mode"]==1&&mode.positionName()=="INTERNAL");
    auto* h=mode.getAccessibilityHandler();
    CHECK(h!=nullptr&&h->getRole()==juce::AccessibilityRole::toggleButton&&h->getValueInterface()!=nullptr
        &&h->getValueInterface()->getCurrentValueAsString()=="INTERNAL"&&h->getCurrentState().isChecked());
}
void tabOrder()
{
    std::cout<<"SECTION tab_order\n";
    Rig rig;
    const juce::StringArray want{"session.display","session.save","session.load","session.browse","session.undo","session.redo",
        "session.rack","session.ai","strip.fold","strip.mode","strip.display","strip.export"};
    juce::StringArray got;
    juce::KeyboardFocusTraverser traverser;
    auto* c=rig.session.control("session.display");
    for(int i=0;i<want.size()&&c!=nullptr;++i){got.add(c->getProperties()["element"].toString());c=traverser.getNextComponent(c);}
    CHECK(got==want);
    if(got!=want)std::cout<<"Tab order: "<<got.joinIntoString(", ")<<'\n';
}
void lettering()
{
    std::cout<<"SECTION lettering\n";
    Rig rig;
    const auto& m=rig.model;
    CHECK(m.face->getName()=="Inconsolata");
    auto& d=*rig.session.display;
    d.show(drawnSession(m));
    rig.strip.display->show(drawnStrip());
    const auto img=snap(rig.session),stripImg=snap(rig.strip);
    auto probe=[&](const juce::Image& im,const juce::String& view,const juce::String& key)
    {
        const auto& l=m.displays.at(view).line(key);
        const auto got=inkBox(im,l.ink.expanded(6.0f,4.0f),90.0);
        const auto e=std::max({std::abs(got.getX()-l.ink.getX()),std::abs(got.getY()-l.ink.getY()),
            std::abs(got.getRight()-l.ink.getRight()),std::abs(got.getBottom()-l.ink.getBottom())});
        CHECK(e<=1.01f);
        if(e>1.01f)std::cout<<view<<"."<<key<<" "<<e<<" px\n";
    };
    probe(img,"session","name");
    probe(img,"session","tempo.host");
    probe(stripImg,"strip","settings");
    const auto ellipsis=juce::String::charToString((juce::juce_wchar)0x2026);
    auto s=drawnSession(m);
    s.name="AN EXTRAORDINARILY LONG PRESET NAME FOR A DISPLAY";
    d.show(s);
    const auto& spec=m.displays.at("session");
    const auto& p=d.placed["name"];
    CHECK(p.shown==juce::String("AN EXTRAORDINARILY")+ellipsis&&std::abs(p.hscale-0.8f)<1e-3f);
    CHECK(p.hscale>=spec.minHscale-1e-4f&&p.path.getBounds().getRight()<=spec.line("name").maxRight+0.5f);
    s.name="CAF"+juce::String::charToString((juce::juce_wchar)0xC9);
    d.show(s);
    CHECK(d.placed["name"].shown==s.name&&m.face->getNominalGlyphForCodepoint((juce::juce_wchar)0xC9).has_value());
    s.name=juce::String::charToString((juce::juce_wchar)0x65E5);
    d.show(s);
    CHECK(d.placed["name"].shown=="?");
}
void overviewCells()
{
    std::cout<<"SECTION overview_cells\n";
    Rig rig;
    const auto& m=rig.model;
    const auto& o=m.overview;
    auto& ov=*rig.strip.overview;
    ov.show(m.mockup.notes,4,4,4);
    const auto cells=studio::overviewCells(m.mockup.notes,o,4,4,4);
    CHECK((int)cells.size()==m.mockup.mappedCells&&cells.size()==140&&ov.cells==cells);
    CHECK((studio::overviewCells({{0.0,0.25,60}},o,4,4,4)==std::set<std::pair<int,int>>{{0,7},{0,8},{1,7},{1,8}}));
    CHECK(studio::overviewCells({{0.0,0.25,60}},o,8,4,4).size()==2);
    int lo=99,hi=-1;
    for(auto& [c,r]:studio::overviewCells({{0.0,1.0,40},{0.0,1.0,90}},o,4,4,4)){lo=std::min(lo,r);hi=std::max(hi,r);}
    CHECK(lo==0&&hi==o.rows-1);
    auto* h=ov.getAccessibilityHandler();
    CHECK(h!=nullptr&&h->getRole()==juce::AccessibilityRole::image&&h->getDescription()=="4 bars, "+juce::String((int)m.mockup.notes.size())+" notes");
    CHECK(m.mockup.notes.size()==34);
    ov.show({},4,4,4);
    const auto plate=snap(rig.strip);
    ov.show({{1.0,0.5,60}},4,4,4);
    const auto lit=ov.cells;
    const auto blk=ov.blocks.front();
    const auto img=snap(rig.strip);
    const auto& l=o.lattice;
    const auto r0=blk.top,r1=blk.top+o.rowsPerNote-1;
    const auto X0=l.origin.x+(blk.x0-0.5f)*l.pitch.x,X1=l.origin.x+(blk.x1-0.5f)*l.pitch.x;
    const auto Y0=l.origin.y+((float)r0-0.5f)*l.pitch.y,Y1=l.origin.y+((float)r1+0.5f)*l.pitch.y;
    const auto plateau=luma(o.lit);
    const auto coverIn=[&](const juce::Image& im,int x,int y){const auto b=luma(plate.getPixelAt(x,y));return (luma(im.getPixelAt(x,y))-b)/std::max(1.0,plateau-b);};
    const auto cover=[&](int x,int y){return coverIn(img,x,y);};
    const auto crossing=[](const std::vector<double>& a,int first,bool rising)
    {
        for(size_t k=0;k+1<a.size();++k)
        {
            const auto i=rising?k:a.size()-2-k;
            if(rising?(a[i]<0.5&&a[i+1]>=0.5):(a[i]>=0.5&&a[i+1]<0.5))return (double)first+(double)i+(0.5-a[i])/(a[i+1]-a[i]);
        }
        return -1.0e9;
    };
    const auto yc=juce::roundToInt(l.origin.y+(float)r0*l.pitch.y),yc1=juce::roundToInt(l.origin.y+(float)r1*l.pitch.y);
    const auto xc=juce::roundToInt((X0+X1)*0.5f);
    const auto xf=(int)std::floor(X0)-3,yf=(int)std::floor(Y0)-3;
    std::vector<double> hp,vp;
    for(int x=xf;x<=(int)std::ceil(X1)+3;++x)hp.push_back(cover(x,yc));
    for(int y=yf;y<=(int)std::ceil(Y1)+3;++y)vp.push_back(cover(xc,y));
    const std::array<double,4> got{crossing(hp,xf,true)-X0,crossing(vp,yf,true)-Y0,X1-crossing(hp,xf,false),Y1-crossing(vp,yf,false)};
    double insetErr=0.0;
    for(size_t i=0;i<4;++i)insetErr=std::max(insetErr,std::abs(got[i]-o.litInset[i]));
    double seamMin=1.0;
    for(int y=yc+1;y<yc1;++y)seamMin=std::min(seamMin,cover(xc,y));
    const auto plateauErr=std::abs(luma(img.getPixelAt(xc,yc))-plateau);
    CHECK(lit.size()==8&&insetErr<=0.35&&std::abs((1.0-seamMin)-o.seamDepth)<=0.1&&plateauErr<=8.0);
    ov.show({{1.0,0.25,60},{1.25,0.25,60}},4,4,4);
    const auto pair=snap(rig.strip);
    const auto boundary=l.origin.x+(ov.blocks.back().x0-0.5f)*l.pitch.x;
    double gapMin=1.0;
    for(auto x=(int)std::floor(boundary-1.5f);x<=(int)std::ceil(boundary+1.5f);++x)gapMin=std::min(gapMin,coverIn(pair,x,yc));
    CHECK(ov.cells.size()==8&&gapMin<0.5);
    std::vector<studio::Note> steps;
    for(int k=0;k<16;++k)steps.push_back({k*0.25,0.25,k%2==0?60:64});
    ov.show(steps,4,4,4);
    const auto bar=snap(rig.strip);
    double wMin=1.0e9,wMax=-1.0e9;
    for(const auto& b:ov.blocks)
    {
        const auto y=juce::roundToInt(l.origin.y+(float)b.top*l.pitch.y);
        const auto xa=(int)std::floor(l.origin.x+(b.x0-0.5f)*l.pitch.x)-3,xb=(int)std::ceil(l.origin.x+(b.x1-0.5f)*l.pitch.x)+3;
        std::vector<double> prof;
        for(int x=xa;x<=xb;++x)prof.push_back(coverIn(bar,x,y));
        const auto width=crossing(prof,xa,false)-crossing(prof,xa,true);
        wMin=std::min(wMin,width);wMax=std::max(wMax,width);
    }
    CHECK(ov.blocks.size()==16&&wMax-wMin<=0.5);
    h=ov.getAccessibilityHandler();
    CHECK(h!=nullptr&&h->getDescription()=="4 bars, 16 notes");
}
void roles()
{
    std::cout<<"SECTION roles\n";
    Rig rig;
    rig.session.display->show(drawnSession(rig.model));
    juce::StringArray wrong;
    for(auto* v:{&rig.session,&rig.strip})
        for(auto& b:v->buttons)
        {
            auto* h=b->getAccessibilityHandler();
            const auto role=b->isToggle()?juce::AccessibilityRole::toggleButton:juce::AccessibilityRole::button;
            if(h==nullptr||h->getRole()!=role||h->getTitle()!=b->element.label)wrong.add(b->element.id);
        }
    auto* dh=rig.session.display->getAccessibilityHandler();
    if(dh==nullptr||dh->getRole()!=juce::AccessibilityRole::staticText||dh->getValueInterface()==nullptr
        ||!dh->getValueInterface()->getCurrentValueAsString().contains(rig.model.mockup.path))
        wrong.add("session.display");
    auto* fh=rig.strip.fold->getAccessibilityHandler();
    if(fh==nullptr||fh->getRole()!=juce::AccessibilityRole::button||!fh->getCurrentState().isExpandable()||fh->getCurrentState().isExpanded())
        wrong.add("strip.fold");
    rig.strip.fold->setExpanded(true);
    if(fh==nullptr||!fh->getCurrentState().isExpanded())wrong.add("strip.fold expanded");
    auto* eh=rig.session.error->getAccessibilityHandler();
    if(eh!=nullptr&&!eh->isIgnored())wrong.add("session.error");
    CHECK(wrong.isEmpty());
    if(!wrong.isEmpty())std::cout<<"wrong roles: "<<wrong.joinIntoString(", ")<<'\n';
}
void todayIds()
{
    std::cout<<"SECTION today_ids\n";
    Rig rig;
    const std::pair<const char*,const char*> buttons[]{{"preset.save","session.save"},{"preset.load","session.load"},
        {"preset.browse","session.browse"},{"undo","session.undo"},{"redo","session.redo"},{"ai.toggle","session.ai"},
        {"rack.toggle","session.rack"},{"strip.mode","strip.mode"},{"strip.export","strip.export"}};
    for(const auto& [id,element]:buttons)
    {
        auto* b=dynamic_cast<juce::Button*>(find(rig,id));
        CHECK(b!=nullptr&&b->getProperties()["element"].toString()==element);
    }
    CHECK(dynamic_cast<studio::Display*>(find(rig,"session.display"))!=nullptr);
    CHECK(dynamic_cast<studio::Display*>(find(rig,"strip.display"))!=nullptr);
    CHECK(dynamic_cast<studio::Overview*>(find(rig,"strip.overview"))!=nullptr);
    CHECK(dynamic_cast<studio::Disclosure*>(find(rig,"strip.fold"))!=nullptr);
    CHECK(dynamic_cast<studio::Indicator*>(find(rig,"session.error"))!=nullptr);
    CHECK(dynamic_cast<juce::TextEditor*>(find(rig,"preset.path"))!=nullptr);
}
void sessionDrawnState()
{
    std::cout<<"SECTION session_drawn_state\n";
    SessionRig rig;
    const auto s=rig.shown();
    CHECK(s.layout=="name"&&s.name=="WARM CIRCUIT"&&s.tempoKey=="host"&&s.tempoText=="120 BPM HOST"&&!s.errorLed&&!rig.errorLit());
    CHECK(rig.display().accessibleValue().contains(rig.warm));
    studio::SessionUnit bare(rig.p,rig.model(),rig.services.make());
    CHECK(bare.getWidth()==1547&&bare.getHeight()==107);
}
void sessionLoadError()
{
    std::cout<<"SECTION session_load_error\n";
    SessionRig rig;
    const char junk[]{1,2,3,4,5};
    rig.p.setStateInformation(junk,5);
    auto s=rig.shown();
    CHECK(s.layout=="message"&&s.headline=="LOAD FAILED: UNSUPPORTED DATA"&&s.detail=="WARM CIRCUIT KEPT"&&s.errorLed&&rig.errorLit());
    CHECK(rig.display().accessibleValue().contains(loadErrorSentence)&&rig.label("state.load.error")==loadErrorSentence);
    const auto& placed=rig.display().placed;
    const auto tempo=placed.find("tempo.host.message");
    CHECK(placed.at("headline").shown==s.headline&&tempo!=placed.end()
        &&!placed.at("headline").path.getBounds().expanded(4.0f).intersects(tempo->second.path.getBounds()));
    juce::MemoryBlock good;
    {InstrumentProcessor fresh{airack::testing::isolatedRackRuntime()};fresh.getStateInformation(good);}
    rig.p.setStateInformation(good.getData(),(int)good.getSize());
    s=rig.shown();
    CHECK(s.layout=="name"&&!s.errorLed&&!rig.errorLit()&&rig.label("state.load.error").isEmpty());
}
void sessionNoHostTempo()
{
    std::cout<<"SECTION session_no_host_tempo\n";
    SessionRig rig(std::nullopt);
    const auto& lines=rig.model().displays.at("session").lines;
    const auto amberBox=Rect(16.0f,16.0f).withCentre(lines.at("tempo.waiting").indicatorCentre);
    auto synced=document(rig.p);
    physical(synced,"synth.1.lfo.1.synced",1);physical(synced,"synth.1.modulation.1.enabled",1);
    physical(synced,"synth.1.modulation.1.source",3);physical(synced,"synth.1.modulation.1.target",1);
    CHECK(restore(rig.p,state(synced)));rig.p.prepareToPlay(48000,64);rig.block(true);
    CHECK(rig.p.tempoUnavailable());
    auto s=rig.shown();
    const auto waiting=meanChannels(snap(*rig.unit),amberBox);
    CHECK(s.tempoKey=="waiting"&&s.tempoText=="NO HOST TEMPO"&&waiting[0]>waiting[2]+60.0);
    CHECK(rig.display().accessibleValue().contains("Host tempo unavailable")&&rig.label("tempo.status").contains("Host tempo unavailable"));
    InstrumentProcessor plain{airack::testing::isolatedRackRuntime()};CHECK(blankInstance(plain));
    CHECK(restore(rig.p,state(document(plain))));rig.p.prepareToPlay(48000,64);rig.block(true);
    CHECK(!rig.p.tempoUnavailable());
    s=rig.shown();
    const auto quiet=meanChannels(snap(*rig.unit),amberBox);
    CHECK(s.tempoKey=="none"&&s.tempoText=="NO HOST TEMPO"&&quiet[0]<quiet[2]+15.0);
    CHECK(rig.display().accessibleValue().contains("Offline / manual controls available"));
}
void sessionMessages()
{
    std::cout<<"SECTION session_messages\n";
    SessionRig rig;
    rig.path(rig.blocked());
    click(*rig.unit,"preset.save");
    auto s=rig.shown();
    CHECK(s.layout=="message"&&s.headline=="SAVE FAILED"&&s.detail=="WARM CIRCUIT KEPT"&&s.errorLed&&rig.errorLit());
    CHECK(rig.label("status")=="Save preset failed; sound preserved");
    rig.services.now+=rig.hold("{op} failed; sound preserved")+1;
    s=rig.shown();
    CHECK(s.layout=="name"&&s.name=="WARM CIRCUIT"&&!s.errorLed&&!rig.errorLit());
    rig.path(rig.warm);
    auto changed=document(rig.p);changed.values[0]=.37f;CHECK(edit(rig.p,changed));
    click(*rig.unit,"undo");
    s=rig.shown();
    CHECK(s.layout=="message"&&s.headline=="UNDONE"&&s.detail=="WARM CIRCUIT"&&!s.errorLed);
    rig.services.now+=1501;
    CHECK(rig.shown().layout=="name");
    rig.path("Warm Circuit.airack");
    click(*rig.unit,"preset.save");
    s=rig.shown();
    CHECK(s.headline=="SELECTION CANCELLED"&&!s.errorLed&&!rig.errorLit());
    rig.services.now+=5000;
    rig.services.answer=juce::File();
    click(*rig.unit,"preset.browse");
    s=rig.shown();
    CHECK(s.headline=="SELECTION CANCELLED"&&!s.errorLed&&!rig.errorLit());
    CHECK(rig.services.requests.size()==2);
    if(!rig.services.requests.empty())
    {
        const auto& r=rig.services.requests.back();
        CHECK(r.title=="Choose a sound preset"&&r.patterns=="*.airack;*.soundstate"&&!r.saving&&!r.osDialog&&r.parent==rig.unit.get());
    }
    rig.services.now+=5000;
    rig.path("");
    CHECK(rig.shown().name=="INIT");
}
void sessionMessageOrder()
{
    std::cout<<"SECTION session_message_order\n";
    SessionRig rig;
    const char junk[]{1,2,3,4,5};
    rig.p.setStateInformation(junk,5);
    auto changed=document(rig.p);changed.values[0]=.37f;CHECK(edit(rig.p,changed));
    const auto good=rig.warm;
    rig.path(rig.blocked());
    click(*rig.unit,"preset.save");
    CHECK(rig.shown().headline=="SAVE FAILED");
    rig.services.now+=200;
    click(*rig.unit,"undo");
    CHECK(rig.shown().headline=="UNDONE");
    rig.services.now+=1499;
    CHECK(rig.shown().headline=="UNDONE");
    rig.services.now+=2;
    auto s=rig.shown();
    CHECK(s.layout=="message"&&s.headline=="LOAD FAILED: UNSUPPORTED DATA"&&s.errorLed);
    rig.services.now+=4000;
    s=rig.shown();
    CHECK(s.headline=="LOAD FAILED: UNSUPPORTED DATA"&&rig.errorLit());
    rig.path(good);
}
void sessionUnusualNames()
{
    std::cout<<"SECTION session_unusual_names\n";
    SessionRig rig;
    const auto& spec=rig.model().displays.at("session");
    rig.path(rig.dir.getChildFile(juce::String::repeatedString("A",200)+".airack").getFullPathName());
    auto s=rig.shown();
    const auto& p=rig.display().placed.at("name");
    CHECK(s.name==juce::String::repeatedString("A",200));
    CHECK(p.shown.endsWithChar((juce::juce_wchar)0x2026)&&std::abs(p.hscale-0.8f)<1e-3f&&p.path.getBounds().getRight()<=spec.line("name").maxRight+0.5f);
    const auto eAcute=juce::String::charToString((juce::juce_wchar)0xC9),sun=juce::String::charToString((juce::juce_wchar)0x65E5);
    rig.path(rig.dir.getChildFile("CAF"+eAcute+sun+".airack").getFullPathName());
    s=rig.shown();
    CHECK(s.name=="CAF"+eAcute+sun&&rig.display().placed.at("name").shown=="CAF"+eAcute+"?");
    CHECK(rig.display().accessibleValue().contains("CAF"+eAcute+sun));
}
// The studio feedback spec's decision 11 in SESSION. With --images: session-named.png and session-name-copied.png.
void sessionSoundName()
{
    std::cout<<"SECTION session_sound_name\n";
    SessionRig rig;
    // A named sound shows its name over the path's stem; a right-click copies it as written.
    auto named=document(rig.p);named.name="Ethereal Rolling Bass";CHECK(restore(rig.p,state(named)));
    auto s=rig.shown();CHECK(s.layout=="name"&&s.name=="ETHEREAL ROLLING BASS");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("session-named.png"),snap(*rig.unit)));
    auto& d=rig.display();const auto at=d.getLocalBounds().getCentre().toFloat();
    d.mouseDown(event(d,at,juce::ModifierKeys::rightButtonModifier,1,false));d.mouseUp(event(d,at,{},1,false));
    s=rig.shown();CHECK(rig.services.copied=="Ethereal Rolling Bass");
    CHECK(s.layout=="message"&&s.headline=="NAME COPIED"&&s.detail=="ETHEREAL ROLLING BASS"&&!s.errorLed&&rig.label("status")=="Name copied");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("session-name-copied.png"),snap(*rig.unit)));
    rig.services.now+=rig.hold("Name copied")+1;CHECK(rig.shown().layout=="name");
    // The path entry is the processor's preset path, which APPLY sets; a reopened SESSION shows it.
    const auto applied=rig.dir.getChildFile("Ethereal Rolling Bass.airack").getFullPathName();rig.p.setSoundPresetPath(applied);
    auto* entry=widget<juce::TextEditor>(*rig.unit,"preset.path");rig.unit->refresh();CHECK(entry!=nullptr&&entry->getText()==applied);
    // A path set on the hidden entry survives a refresh that comes before its change message.
    if(entry){entry->setText(rig.warm);rig.unit->refresh();pump();}CHECK(rig.p.soundPresetPath()==rig.warm);
    studio::SessionUnit reopened(rig.p,rig.model(),rig.services.make());
    auto* again=widget<juce::TextEditor>(reopened,"preset.path");CHECK(again!=nullptr&&again->getText()==rig.warm);
}
void sessionPathEntry()
{
    std::cout<<"SECTION session_path_entry\n";
    SessionRig rig;
    auto& d=rig.display();
    auto* entry=widget<juce::TextEditor>(*rig.unit,"preset.path");
    if(entry==nullptr)return;
    CHECK(!entry->isVisible());
    d.grabKeyboardFocus();
    d.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(entry->isVisible()&&entry->getText()==rig.warm);
    const auto glass=rig.dir.getChildFile("GLASS ORGAN.airack").getFullPathName();
    entry->setText(glass);
    entry->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(wait("Return commits the preset path",[&]{return !entry->isVisible()&&d.session.name=="GLASS ORGAN";}));
    // Drain the commit's posted focus loss before beginning another edit on the same entry.
    const auto settled=std::make_shared<bool>(false);
    juce::MessageManager::callAsync([settled]{*settled=true;});
    CHECK(wait("Return focus messages settle",[&]{return *settled;}));
    CHECK(!entry->isVisible()&&rig.shown().name=="GLASS ORGAN");
    d.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(entry->isVisible()&&entry->getText()==glass);
    entry->setText(rig.dir.getChildFile("ELSEWHERE.airack").getFullPathName());
    // Deliver Escape behind one occupied message turn, as the posted-click witness does.
    juce::MessageManager::callAsync([safe=juce::Component::SafePointer<juce::TextEditor>(entry)]
    {
        juce::Thread::sleep(150);
        if(safe!=nullptr)safe->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    });
    CHECK(wait("Escape restores the committed preset path",[&]{return !entry->isVisible()&&d.session.name=="GLASS ORGAN"&&entry->getText()==glass;}));
    CHECK(!entry->isVisible()&&rig.shown().name=="GLASS ORGAN"&&entry->getText()==glass);
    juce::Component::unfocusAllComponents();
}
// SAVE, LOAD and BROWSE act on what the open entry holds, whether pressed for accessibility, which leaves the focus in
// the entry, or clicked. A click is played in JUCE's order: the focus moves to SAVE, SAVE sees the press, and the
// entry's posted focus loss arrives before the release.
void sessionTypedPath()
{
    std::cout<<"SECTION session_typed_path\n";
    SessionRig rig;
    auto* entry=widget<juce::TextEditor>(*rig.unit,"preset.path");
    auto* save=widget<studio::SkinButton>(*rig.unit,"preset.save");
    auto* load=widget<studio::SkinButton>(*rig.unit,"preset.load");
    auto* browse=widget<studio::SkinButton>(*rig.unit,"preset.browse");
    if(entry==nullptr||save==nullptr||load==nullptr||browse==nullptr)return;
    auto type=[&](const char* name)
    {
        rig.display().openEntry();
        CHECK(wait("the open entry has the keyboard focus",[&]{return entry->hasKeyboardFocus(false);}));
        entry->setText(rig.dir.getChildFile(name).getFullPathName());
    };
    auto accessiblePress=[&](studio::SkinButton& b)
    {
        auto* h=b.getAccessibilityHandler();
        CHECK(h!=nullptr&&h->getActions().invoke(juce::AccessibilityActionType::press));
        pump();
    };
    type("COLD CIRCUIT.airack");
    accessiblePress(*save);
    CHECK(rig.dir.getChildFile("COLD CIRCUIT.airack").existsAsFile()&&!juce::File(rig.warm).existsAsFile());
    CHECK(!entry->isVisible()&&rig.shown().name=="COLD CIRCUIT");
    type("GLASS ORGAN.airack");
    const auto at=local(*save,save->element.rect.getCentre());
    save->grabKeyboardFocus();
    CHECK(save->hasKeyboardFocus(false));
    save->mouseDown(event(*save,at,juce::ModifierKeys::leftButtonModifier,1,false));
    pump();
    save->mouseUp(event(*save,at,{},1,false));
    CHECK(rig.dir.getChildFile("GLASS ORGAN.airack").existsAsFile()&&!entry->isVisible()&&rig.shown().name=="GLASS ORGAN");
    type("MISSING.airack");
    accessiblePress(*load);
    CHECK(rig.label("status")=="Load preset failed; sound preserved"&&rig.shown().name=="GLASS ORGAN");
    rig.services.answer=juce::File();
    type("COLD CIRCUIT.airack");
    accessiblePress(*browse);
    CHECK(rig.services.requests.size()==1&&rig.shown().name=="GLASS ORGAN");
    juce::Component::unfocusAllComponents();
}
void sessionAiAndLever()
{
    std::cout<<"SECTION session_ai_and_lever\n";
    SessionRig rig;
    int aiCalls=0,leverCalls=0;
    bool aiOpen=false,advanced=false;
    rig.unit->onAi=[&](bool open){++aiCalls;aiOpen=open;};
    rig.unit->onLever=[&](bool a){++leverCalls;advanced=a;};
    auto* ai=widget<studio::SkinButton>(*rig.unit,"ai.toggle");
    auto* rack=widget<studio::Lever>(*rig.unit,"rack.toggle");
    if(ai==nullptr||rack==nullptr)return;
    press(*ai);
    CHECK(aiCalls==1&&aiOpen&&!ai->lit());
    const auto ledBox=Rect(2*ai->element.lensR,2*ai->element.lensR).withCentre(ai->element.led);
    const auto off=meanLuma(snap(*rig.unit),ledBox);
    rig.unit->setAiOpen(true);
    const auto on=meanLuma(snap(*rig.unit),ledBox);
    auto* h=ai->getAccessibilityHandler();
    CHECK(ai->lit()&&on>off+40.0&&h!=nullptr&&h->getCurrentState().isChecked()&&aiCalls==1);
    CHECK(std::abs(off-86.8)<0.5&&std::abs(on-179.8)<0.5);
    press(*ai);
    CHECK(aiCalls==2&&!aiOpen);
    clickAt(*rack,rack->element.pivot);
    CHECK(leverCalls==1&&advanced&&rack->position()==1);
    rig.unit->setLever(false);
    CHECK(leverCalls==1&&rack->position()==0);
    rig.unit->setLever(true);
    CHECK(leverCalls==1&&rack->position()==1);
}
void sessionChooserOutlivesEditor()
{
    std::cout<<"SECTION session_chooser_outlives_editor\n";
    SessionRig rig;
    rig.services.hold=true;
    click(*rig.unit,"preset.browse");
    CHECK(rig.services.held!=nullptr);
    rig.unit.reset();
    if(rig.services.held)rig.services.held(rig.dir.getChildFile("LATE.airack"));
    pump();
    CHECK(rig.unit==nullptr);
}
void sessionKeepsTodaysSentences()
{
    std::cout<<"SECTION session_keeps_todays_sentences\n";
    SessionRig rig;
    click(*rig.unit,"preset.save");
    CHECK(rig.label("status")=="Save preset complete"&&juce::File(rig.warm).existsAsFile());
    CHECK(rig.shown().headline=="PRESET SAVED");
    click(*rig.unit,"preset.load");
    CHECK(rig.label("status")=="Load preset complete"&&rig.unit->sentence()=="Load preset complete");
    rig.unit->report("Rack capacity reached");
    CHECK(rig.shown().headline=="RACK FULL"&&rig.unit->sentence()=="Rack capacity reached");
    SessionRig fresh;
    click(*fresh.unit,"undo");
    CHECK(fresh.label("status")=="Undo failed; sound preserved");
}
void sessionFirstSaveChooser()
{
    std::cout<<"SECTION session_first_save_chooser\n";
    {
        SessionRig rig;rig.path("");const auto before=document(rig.p);const auto file=rig.dir.getChildFile("First sound.airack");rig.services.answer=file;
        click(*rig.unit,"preset.save");CHECK(rig.services.requests.size()==1);
        if(!rig.services.requests.empty()){const auto& r=rig.services.requests.back();CHECK(r.saving&&r.osDialog&&r.patterns=="*.airack"&&r.parent==rig.unit.get());CHECK(juce::File::isAbsolutePath(r.initial.getFullPathName())&&r.initial.hasFileExtension("airack"));}
        CHECK(file.existsAsFile());CHECK(rig.p.soundPresetPath()==file.getFullPathName());CHECK(rig.label("status")=="Save preset complete");
        auto expected=before;expected.name="First sound";CHECK(document(rig.p)==expected);
        InstrumentProcessor recalled{airack::testing::isolatedRackRuntime()};CHECK(recalled.loadSoundPreset(file));CHECK(document(recalled)==expected);
    }
    {
        SessionRig rig;rig.path("");const auto before=document(rig.p);click(*rig.unit,"preset.save");
        CHECK(rig.services.requests.size()==1&&rig.p.soundPresetPath().isEmpty());CHECK(document(rig.p)==before);
    }
    {
        SessionRig rig;rig.path("");rig.services.hold=true;click(*rig.unit,"preset.save");CHECK(rig.services.held!=nullptr);
        const auto late=rig.dir.getChildFile("Late.airack");rig.unit.reset();if(rig.services.held)rig.services.held(late);pump();CHECK(!late.existsAsFile());
    }
    {
        SessionRig rig;rig.path("");rig.services.hold=true;click(*rig.unit,"preset.save");CHECK(rig.services.held!=nullptr);
        auto replacement=document(rig.p);replacement.values[0]=.37f;CHECK(restore(rig.p,state(replacement)));
        const auto late=rig.dir.getChildFile("Recalled.airack");if(rig.services.held)rig.services.held(late);pump();CHECK(!late.existsAsFile()&&document(rig.p)==replacement);
    }
}
void sessionFactorySounds()
{
    std::cout<<"SECTION session_factory_sounds\n";
    SessionRig rig;juce::StringArray labels;std::function<void(int)> answer;int menus=0;
    const auto wire=[&]{auto services=rig.services.make();
        services.chooseMenu=[&](juce::Component* parent,const juce::StringArray& items,std::function<void(int)> done){
            CHECK(parent==find(*rig.unit,"preset.browse"));labels=items;answer=std::move(done);++menus;};
        rig.unit=std::make_unique<studio::SessionUnit>(rig.p,rig.model(),std::move(services));rig.unit->addToDesktop(0);rig.unit->setVisible(true);};
    wire();ProjectDocument project;std::uint64_t revision=0;CHECK(rig.p.tryProjectSnapshot(project,revision));
    auto phrase=settings(2,{4,4},PatternGrid::sixteenth,0);phrase.noteCount=2;
    phrase.notes[0]={0,.5,48,90,1};phrase.notes[1]={1.5,.25,55,105,2};CHECK(rig.p.applyPatternEdit(phrase,revision));
    CHECK(rig.p.tryProjectSnapshot(project,revision));CHECK(rig.p.setPlaybackMode(PlaybackMode::internalPattern,revision));
    CHECK(rig.p.tryProjectSnapshot(project,revision));const auto before=document(rig.p);
    click(*rig.unit,"preset.browse");CHECK(menus==1&&labels.size()==13&&answer);
    click(*rig.unit,"preset.browse");CHECK(menus==1);
    if(answer)answer(0);CHECK(document(rig.p)==before&&rig.p.soundPresetPath()==rig.warm);
    CHECK(rig.services.requests.empty());
    for(std::size_t i=0;i<factorySounds().size();++i){
        click(*rig.unit,"preset.browse");if(labels.size()==13)CHECK(labels[int(i)+1]==factorySounds()[i].name);
        if(answer)answer(int(i)+2);SoundDocument expected;CHECK(makeFactorySound(i,expected));
        CHECK(document(rig.p)==expected&&rig.p.soundPresetPath().isEmpty());
        ProjectDocument after;CHECK(rig.p.tryProjectSnapshot(after,revision));CHECK(after.pattern==project.pattern&&after.mode==project.mode);
    }
    CHECK(rig.p.undoSoundEdit());CHECK(rig.p.soundName()==factorySounds()[10].name);CHECK(rig.p.redoSoundEdit());
    const auto saved=rig.dir.getChildFile("My sound.airack");rig.services.answer=saved;click(*rig.unit,"preset.save");
    CHECK(saved.existsAsFile()&&!rig.services.requests.empty()&&rig.services.requests.back().saving);
    const auto prior=document(rig.p);CHECK(!rig.p.loadFactorySound(factorySounds().size())&&document(rig.p)==prior);
    rig.services.answer={};const auto requests=rig.services.requests.size();click(*rig.unit,"preset.browse");if(answer)answer(1);
    CHECK(rig.services.requests.size()==requests+1&&!rig.services.requests.back().saving);
    click(*rig.unit,"preset.browse");auto changed=document(rig.p);changed.values[0]=.23f;CHECK(edit(rig.p,changed));
    if(answer)answer(2);CHECK(document(rig.p)==changed);
    click(*rig.unit,"preset.browse");auto recalled=changed;recalled.values[0]=.31f;CHECK(restore(rig.p,state(recalled)));
    if(answer)answer(2);CHECK(document(rig.p)==recalled);
    click(*rig.unit,"preset.browse");auto retired=answer;rig.unit.reset();if(retired)retired(2);CHECK(document(rig.p)==recalled);
    {
        SessionRig actual;auto services=actual.services.make();services.chooseMenu=studio::defaultStudioServices().chooseMenu;
        actual.unit=std::make_unique<studio::SessionUnit>(actual.p,actual.model(),std::move(services));
        actual.unit->addToDesktop(0);actual.unit->setVisible(true);const auto unchanged=document(actual.p);
        const auto modalCount=juce::Component::getNumCurrentlyModalComponents();click(*actual.unit,"preset.browse");
        CHECK(juce::Component::getNumCurrentlyModalComponents()==modalCount+1);
        if(auto* popup=juce::Component::getCurrentlyModalComponent())
            if(output!=juce::File{})CHECK(writePng(output.getChildFile("factory-sound-menu.png"),snap(*popup)));
        CHECK(juce::PopupMenu::dismissAllActiveMenus());pump();
        CHECK(juce::Component::getNumCurrentlyModalComponents()==modalCount&&document(actual.p)==unchanged);
    }
}
void stripSettings()
{
    std::cout<<"SECTION strip_settings\n";
    StripRig rig;
    rig.pattern(settings(4,{4,4},PatternGrid::sixteenth,0));
    auto s=rig.shown();
    CHECK(s.layout=="settings"&&s.text=="4 BARS  4/4  1/16  SWING 0%"&&!s.amber);
    rig.pattern(settings(1,{7,8},PatternGrid::thirtySecond,0.5));
    s=rig.shown();
    CHECK(s.layout=="settings"&&s.text=="1 BAR  7/8  1/32  SWING 50%");
}
void stripPlayback()
{
    std::cout<<"SECTION strip_playback\n";
    {
        StripRig rig(false);
        auto s=rig.shown();
        CHECK(s.layout=="settings"&&!s.amber&&rig.status()=="External MIDI / keyboard available");
        rig.mode(PlaybackMode::internalPattern);
        s=rig.shown();
        CHECK(s.layout=="alert"&&s.text=="WAITING FOR AUDIO"&&s.amber);
    }
    {
        StripRig rig;
        rig.head.available=false;
        rig.p.setPlayHead(&rig.head);
        rig.mode(PlaybackMode::internalPattern);
        rig.block();
        const auto s=rig.shown();
        auto* h=rig.display().getAccessibilityHandler();
        CHECK(s.layout=="alert"&&s.text=="WAITING FOR HOST TIMING"&&s.amber);
        CHECK(h!=nullptr&&h->getValueInterface()!=nullptr
              &&h->getValueInterface()->getCurrentValueAsString().startsWith("Internal Pattern waiting for host timing (tempo and beat position). "));
    }
    for(auto mode:{PlaybackMode::externalMidi,PlaybackMode::internalPattern})
    {
        StripRig rig;
        if(mode==PlaybackMode::internalPattern)rig.mode(mode);
        rig.block();
        int queued=0;
        for(int i=0;i<256;++i)queued+=rig.p.enqueueKeyboardNote(true,60,100,1)?1:0;
        CHECK(queued==256&&!rig.p.enqueueKeyboardNote(true,60,100,1));
        const auto s=rig.shown();
        CHECK(s.layout=="alert"&&s.text=="INPUT FULL: NOTES RELEASED"&&s.amber);
        CHECK(rig.status()=="Input capacity reached; notes released. Try fewer simultaneous events.");
        rig.p.releaseKeyboardNotes();
    }
}
void stripExport()
{
    std::cout<<"SECTION strip_export\n";
    StripRig rig;
    rig.pattern(mockupPattern(rig.model()));
    const auto x=rig.dir.getChildFile("x.mid");
    rig.services.answer=x;
    click(*rig.strip,"strip.export");
    CHECK(rig.services.requests.size()==1);
    if(!rig.services.requests.empty())
    {
        const auto& r=rig.services.requests.back();
        CHECK(r.title=="Pattern Export"&&r.patterns=="*.mid"&&r.saving&&r.osDialog&&r.parent==rig.strip.get());
    }
    CHECK(x.existsAsFile()&&x.getSize()>30);
    auto s=rig.shown();
    CHECK(s.layout=="settings"&&s.text=="MIDI EXPORTED"&&!s.amber&&rig.status().startsWith("MIDI export complete | "));
    rig.services.now+=1501;
    s=rig.shown();
    CHECK(s.layout=="settings"&&s.text=="4 BARS  4/4  1/16  SWING 0%");
    rig.services.answer=juce::File();
    click(*rig.strip,"strip.export");
    s=rig.shown();
    CHECK(s.layout=="settings"&&s.text=="EXPORT CANCELLED"&&!s.amber);
    CHECK(rig.status().startsWith("Pattern selection cancelled; committed project preserved | "));
    rig.services.hold=true;
    click(*rig.strip,"strip.export");
    click(*rig.strip,"strip.export");
    s=rig.shown();
    CHECK(rig.services.requests.size()==3&&s.layout=="alert"&&s.text=="CHOOSER ALREADY OPEN"&&s.amber);
    if(rig.services.held)rig.services.held(juce::File());
    rig.services.hold=false;
    click(*rig.strip,"strip.export");
    CHECK(rig.services.requests.size()==4);
}
// TMP, and so the temporary folder, names `folder` until this goes; the C runtime passes it to the process's environment.
struct TempFolder
{
    const juce::String previous=juce::SystemStats::getEnvironmentVariable("TMP",{});
    explicit TempFolder(const juce::File& folder) { point(folder); }
    ~TempFolder() { _wputenv_s(L"TMP",previous.toWideCharPointer()); }
    void point(const juce::File& folder) { CHECK(_wputenv_s(L"TMP",folder.getFullPathName().toWideCharPointer())==0); }
};
// The note-ons in a standard MIDI file, as a DAW reads it; -1 if it does not read.
int midiNotes(const juce::File& file)
{
    juce::FileInputStream in(file);juce::MidiFile midi;int notes=0;
    if(!in.openedOk()||!midi.readFrom(in))return -1;
    for(int t=0;t<midi.getNumTracks();++t)for(const auto* e:*midi.getTrack(t))notes+=e->message.isNoteOn()?1:0;
    return notes;
}
// A typical Windows importer's open: reading, and sharing the file with readers only. A writer's lease would refuse it.
bool importerReads(const juce::File& file)
{
    int fd=-1;
    if(_wsopen_s(&fd,file.getFullPathName().toWideCharPointer(),_O_RDONLY|_O_BINARY,_SH_DENYWR,0)!=0)return false;
    char header[4]{};
    const bool read=_read(fd,header,4)==4&&std::string(header,4)=="MThd";
    _close(fd);
    return read;
}
// The display shows `text` whole, amber or not.
bool showsWhole(studio::Display& d,const studio::StripShown& s,const juce::String& text,bool amber)
{
    const auto at=d.placed.find(s.layout);
    return s.text==text&&s.amber==amber&&at!=d.placed.end()&&at->second.shown==text;
}
// EXPORT dragged 6 px or more leaves the window as the system's file drag of a MIDI snapshot of the committed
// pattern, for a DAW to import, never moved; under 6 px, Return and Space it still asks for a file. The file outlives
// the drag and the strip, and stays readable while nothing may delete it until the drag ends.
void stripExportDrag()
{
    std::cout<<"SECTION strip_export_drag\n";
    StripRig rig;
    const auto temp=rig.dir.getChildFile("temp");
    CHECK(temp.createDirectory());
    TempFolder redirect(temp);
    const auto store=temp.getChildFile("AI Rack Synth MIDI");
    const auto files=[&]{return store.findChildFiles(juce::File::findFiles,false,"airack-*.mid");};
    const auto drawn=[&](const juce::String& text,bool amber){return showsWhole(rig.display(),rig.shown(),text,amber);};
    const auto pattern=mockupPattern(rig.model());
    rig.pattern(pattern);
    auto* b=widget<studio::SkinButton>(*rig.strip,"strip.export");
    CHECK(b!=nullptr);
    if(b==nullptr)return;
    CHECK(b->getTooltip()=="Drag MIDI into your DAW; click or press Return to save a MIDI file");
    const auto at=b->element.rect.getCentre();
    const auto dragOut=[&](studio::SkinButton& button){const auto c=button.element.rect.getCentre();drag(button,c,c+Pt(12,0));};
    const auto before=rig.project();
    rig.services.answer=juce::File();
    drag(*b,at,at+Pt(5,0));
    CHECK(rig.services.drags.empty()&&rig.services.requests.size()==1&&files().isEmpty());
    CHECK(b->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    CHECK(rig.services.drags.empty()&&rig.services.requests.size()==2);
    dragOut(*b);
    const auto exported=files();
    CHECK(exported.size()==1&&rig.services.drags.size()==1&&rig.services.requests.size()==2);
    if(exported.size()!=1||rig.services.drags.size()!=1)return;
    const auto dragged=exported[0];
    const auto& offered=rig.services.drags.back();
    CHECK(offered.files==juce::StringArray(dragged.getFullPathName())&&!offered.canMove&&offered.source==b);
    CHECK(midiNotes(dragged)==(int)pattern.noteCount&&importerReads(dragged));
    CHECK(!dragged.deleteFile()&&dragged.existsAsFile());
    CHECK(rig.project()==before);
    CHECK(rig.status().startsWith("MIDI file ready: drop into your DAW to copy the pattern | ")&&drawn("DROP MIDI IN YOUR DAW",false));
    // One drag at a time, and the file keeps the pattern as it was dragged.
    dragOut(*b);
    CHECK(rig.services.drags.size()==1&&files().size()==1);
    CHECK(rig.status().startsWith("Finish the current MIDI drag first | ")&&drawn("MIDI DRAG IN PROGRESS",true));
    juce::MemoryBlock bytes,still;
    CHECK(dragged.loadFileAsData(bytes));
    auto changed=pattern;
    changed.notes[0].pitch=(std::uint8_t)(changed.notes[0].pitch+1);
    rig.pattern(changed);
    CHECK(dragged.loadFileAsData(still)&&still==bytes);
    // The drop ends the drag; the file stays for the DAW's import, and can then go.
    rig.services.endDrag();
    CHECK(rig.status().startsWith("MIDI drag ended; file retained for DAW import | "));
    CHECK(dragged.existsAsFile()&&importerReads(dragged));
    // A right-button drag is no drag.
    {
        const auto from=local(*b,at),to=local(*b,at+Pt(12,0));
        const juce::ModifierKeys right(juce::ModifierKeys::rightButtonModifier);
        b->mouseDown(event(*b,from,right,1,false));
        b->mouseDrag(event(*b,to,right,1,true,from));
        b->mouseUp(event(*b,to,{},1,true,from));
        CHECK(rig.services.drags.size()==1&&files().size()==1);
    }
    // No notes, no drag.
    rig.pattern(PatternDocument{});
    dragOut(*b);
    CHECK(rig.services.drags.size()==1&&files().size()==1);
    CHECK(rig.status().startsWith("MIDI drag failed: pattern is empty or invalid; add a note first | ")&&drawn("NO NOTES TO DRAG",true));
    rig.pattern(changed);
    // No temporary folder: a file stands where it would be.
    const auto blocked=rig.dir.getChildFile("not a folder");
    CHECK(blocked.replaceWithText("x"));
    redirect.point(blocked);
    dragOut(*b);
    CHECK(rig.services.drags.size()==1&&rig.status().startsWith("MIDI drag failed: cannot create temporary export folder | ")&&drawn("MIDI DRAG FAILED",true));
    redirect.point(temp);
    // The system refuses the drag: the file it would have offered goes too.
    rig.services.dragStarts=false;
    dragOut(*b);
    CHECK(rig.services.drags.size()==2&&files().size()==1);
    CHECK(rig.status().startsWith("MIDI drag failed: native file drag could not start | ")&&drawn("MIDI DRAG FAILED",true));
    rig.services.dragStarts=true;
    // A day old, our files expire before a drag: not one a reader holds open as fopen does, sharing no deletion, and
    // nobody else's.
    const auto old=juce::Time::getCurrentTime()-juce::RelativeTime::hours(25);
    const auto ours=[&]{return store.getChildFile("airack-"+juce::Uuid().toString()+".mid");};
    const auto stale=ours(),held=ours(),unrelated=store.getChildFile("unrelated.mid");
    for(const auto& f:{stale,held,unrelated})CHECK(f.replaceWithText("fixture")&&f.setLastModificationTime(old));
    int reader=-1;
    CHECK(_wsopen_s(&reader,held.getFullPathName().toWideCharPointer(),_O_RDONLY|_O_BINARY,_SH_DENYNO,0)==0);
    dragOut(*b);
    CHECK(rig.services.drags.size()==3);
    CHECK(!stale.exists());
    CHECK(held.existsAsFile());
    CHECK(unrelated.loadFileAsString()=="fixture");
    rig.services.endDrag();
    if(reader>=0)_close(reader);
    dragOut(*b);
    CHECK(rig.services.drags.size()==4&&!held.exists()&&unrelated.existsAsFile());
    rig.services.endDrag();
    // At 128 young files a drag is refused rather than removing one a DAW may still be reading.
    while(files().size()<128)
        if(!ours().replaceWithText("young")){CHECK(false);break;}
    dragOut(*b);
    CHECK(rig.services.drags.size()==4&&files().size()==128);
    CHECK(rig.status().startsWith("MIDI drag storage full: older exports must expire before another drag | ")&&drawn("DRAG STORAGE FULL",true));
    for(const auto& f:files())if(f!=dragged)CHECK(f.deleteFile());
    // The narrow strip drags from its own EXPORT, and a drag that ends after the strip has gone leaves its file.
    rig.strip->setNarrow(true);
    auto* narrow=widget<studio::SkinButton>(*rig.strip,"strip.export");
    CHECK(narrow!=nullptr&&narrow!=b);
    if(narrow==nullptr)return;
    dragOut(*narrow);
    CHECK(rig.services.drags.size()==5&&rig.services.drags.back().source==narrow&&files().size()==2);
    rig.strip.reset();
    rig.services.endDrag();
    pump();
    CHECK(files().size()==2);
}
void stripModeLever()
{
    std::cout<<"SECTION strip_mode_lever\n";
    StripRig rig;
    auto* mode=widget<studio::Lever>(*rig.strip,"strip.mode");
    if(mode==nullptr)return;
    std::uint64_t before=0,after=0;
    rig.project(&before);
    drag(*mode,mode->element.pivot,mode->element.pivot+Pt(0,14));
    const auto edited=rig.project(&after);
    auto* h=mode->getAccessibilityHandler();
    CHECK(edited.mode==PlaybackMode::internalPattern&&after>before&&mode->position()==1);
    CHECK(rig.status().startsWith("Mode edit complete | "));
    CHECK(h!=nullptr&&h->getValueInterface()!=nullptr&&h->getValueInterface()->getCurrentValueAsString()=="INTERNAL");
    ProjectDocument recalled;
    recalled.mode=PlaybackMode::internalPattern;
    recalled.pattern.noteCount=1;
    recalled.pattern.notes[0]={0,.25,64,100,1};
    juce::MemoryBlock bytes;
    CHECK(encodeProjectState(recalled,bytes)&&rig.p.restoreProjectState(bytes.getData(),(int)bytes.getSize()));
    rig.project(&before);
    mode->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    rig.project(&after);
    CHECK(after==before&&rig.project().mode==PlaybackMode::internalPattern&&mode->position()==1);
    CHECK(rig.status().startsWith("Edit cancelled: project changed; select the note again"));
}
void stripOverview()
{
    std::cout<<"SECTION strip_overview\n";
    StripRig rig;
    const auto& m=rig.model();
    rig.pattern(mockupPattern(m));
    auto* overview=widget<studio::Overview>(*rig.strip,"strip.overview");
    if(overview==nullptr)return;
    auto* h=overview->getAccessibilityHandler();
    CHECK(overview->cells.size()==140&&(int)overview->cells.size()==m.mockup.mappedCells);
    CHECK(overview->cells==studio::overviewCells(m.mockup.notes,m.overview,4,4,4));
    CHECK(h!=nullptr&&h->getDescription()=="4 bars, 34 notes");
}
// Narrowed, the strip shows strip.narrow, whose lever, fold and EXPORT MIDI act as the wide strip's; both follow the
// project, so widening shows the wide strip as it now is.
void stripNarrows()
{
    std::cout<<"SECTION strip_narrows\n";
    StripRig rig;
    const auto& m=rig.model();
    rig.pattern(mockupPattern(m));
    auto* wide=widget<studio::Overview>(*rig.strip,"strip.overview");
    auto* wideMode=widget<studio::Lever>(*rig.strip,"strip.mode");
    auto* wideDisplay=widget<studio::Display>(*rig.strip,"strip.display");
    if(wide==nullptr||wideMode==nullptr||wideDisplay==nullptr)return;
    rig.strip->setNarrow(true);
    auto* narrow=widget<studio::Overview>(*rig.strip,"strip.overview");
    auto* mode=widget<studio::Lever>(*rig.strip,"strip.mode");
    if(narrow==nullptr||mode==nullptr)return;
    CHECK(rig.strip->getWidth()==(int)m.layout["strip_narrow"]["width"]&&rig.strip->getHeight()==104&&narrow!=wide&&mode!=wideMode);
    CHECK(narrow->cells==studio::overviewCells(m.mockup.notes,m.overviewNarrow,4,4,4)&&wide->cells.size()==140);
    auto s=rig.shown();
    CHECK(&rig.display()!=wideDisplay&&s.layout=="settings"&&s.text=="4 BARS  4/4  1/16  SWING 0%");
    drag(*mode,mode->element.pivot,mode->element.pivot+Pt(0,14));
    CHECK(rig.project().mode==PlaybackMode::internalPattern&&mode->position()==1&&wideMode->position()==1);
    CHECK(rig.status().startsWith("Mode edit complete | "));
    rig.services.now+=1501;
    s=rig.shown();
    CHECK(s.text.isNotEmpty()&&s.text==wideDisplay->strip.text&&s.layout==wideDisplay->strip.layout&&s.amber==wideDisplay->strip.amber);
    int calls=0;
    bool unfolded=false;
    rig.strip->onFold=[&](bool u){++calls;unfolded=u;};
    if(auto* fold=widget<studio::Disclosure>(*rig.strip,"strip.fold"))fold->press();
    CHECK(calls==1&&unfolded);
    rig.strip->setUnfolded(true);
    const auto x=rig.dir.getChildFile("narrow.mid");
    rig.services.answer=x;
    click(*rig.strip,"strip.export");
    CHECK(x.existsAsFile()&&rig.status().startsWith("MIDI export complete | "));
    rig.strip->setNarrow(false);
    auto* fold=widget<studio::Disclosure>(*rig.strip,"strip.fold");
    CHECK(rig.strip->getWidth()==(int)m.layout["stage"][0]&&widget<studio::Overview>(*rig.strip,"strip.overview")==wide);
    CHECK(fold!=nullptr&&fold->expanded&&wideMode->position()==1);
}
void stripFold()
{
    std::cout<<"SECTION strip_fold\n";
    StripRig rig;
    int calls=0;
    bool unfolded=false;
    rig.strip->onFold=[&](bool u){++calls;unfolded=u;};
    auto* fold=widget<studio::Disclosure>(*rig.strip,"strip.fold");
    if(fold==nullptr)return;
    std::uint64_t before=0,after=0;
    rig.project(&before);
    clickAt(*fold,fold->element.triangle.getCentre());
    rig.project(&after);
    CHECK(calls==1&&unfolded&&after==before);
    rig.strip->setUnfolded(true);
    auto* h=fold->getAccessibilityHandler();
    CHECK(calls==1&&h!=nullptr&&h->getCurrentState().isExpanded());
    fold->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(calls==2&&!unfolded);
}
// A knob binding that logs its calls, over `steps` detents from detent `at`.
struct Recording final:studio::KnobBinding
{
    Recording(std::vector<juce::String>& l,int stepsIn,int at):log(l),n(stepsIn),k(at){}
    int steps() const override{return n;}
    double fraction() const override{return (double)k/n;}
    void set(double f) override{k=(int)std::lround(f*n);log.push_back("set "+juce::String(k));}
    juce::String valueText() const override{return juce::String(k+1)+" BARS";}
    juce::String entryText() const override{return juce::String(k+1);}
    bool enter(const juce::String& t) override{log.push_back("enter "+t);return true;}
    bool enabled() const override{return on;}
    void addMenu(Menu& m) override{m.push_back({"Denominator 8",[this]{log.push_back("menu 8");}});}
    bool begin() override{log.push_back("begin");return true;}
    void end(bool keep) override{log.push_back(keep?"end":"cancel");}
    std::vector<juce::String>& log;
    int n,k;
    bool on=true;
};
// The unfolded unit's view on the desktop, so that focus works.
struct PatternRig final:juce::Component
{
    const studio::Model& model=*studio::studioSkin();
    studio::SkinView view{model,"pattern"};
    std::vector<juce::String> log;
    PatternRig(){addAndMakeVisible(view);setSize(view.getWidth(),view.getHeight());addToDesktop(0);setVisible(true);}
    ~PatternRig() override{removeFromDesktop();}
    studio::Knob& knob(const juce::String& local){return *view.get<studio::Knob>("pattern."+local);}
    Recording& bind(const juce::String& local,int steps,int at)
    {
        auto r=std::make_unique<Recording>(log,steps,at);
        auto& ref=*r;
        knob(local).setBinding(std::move(r));
        return ref;
    }
};
// The pattern unit's drawn state (elements.json's mockup.pattern) as the roll shows it.
studio::RollShown drawnRoll(const studio::Model& m)
{
    const auto& p=m.patternMockup;
    studio::RollShown s;
    s.bars=(int)p["bars"];s.numerator=(int)p["meter"][0];s.denominator=(int)p["meter"][1];s.grid=p["grid"].toString();s.gridBeats=0.25;
    for(const auto& n:*p["notes"].getArray())
        s.notes.push_back({(double)n["start"],(double)n["length"],(int)n["pitch"],(int)n["velocity"],(int)n["channel"]});
    s.selected=(int)p["selected"];
    s.playhead=(double)p["playhead_beats"];
    return s;
}
Pt low(Rect key) { return {key.getCentreX(),key.getBottom()-12.0f}; }
void patternElements()
{
    std::cout<<"SECTION pattern_elements\n";
    PatternRig rig;
    int expected=0,components=0;
    for(const auto& e:rig.model.elements)
        if(e.view=="pattern"){++expected;components+=rig.view.control(e.id)!=nullptr;}
    CHECK(expected==25&&components==expected);
    CHECK(rig.view.getTitle()=="Pattern unit"&&rig.view.getWidth()==1547&&rig.view.getHeight()==702);
}
void patternHitRegions()
{
    std::cout<<"SECTION pattern_hit_regions\n";
    PatternRig rig;
    auto& v=rig.view;
    int ok=0,total=0;
    juce::StringArray misses;
    auto probe=[&](bool passed,const juce::String& what){++total;ok+=passed;if(!passed)misses.add(what);};
    for(auto& k:v.knobs)
    {
        probe(v.getComponentAt(k->centre.toInt())==k.get(),k->element.id+" centre");
        probe(v.getComponentAt(Pt(k->centre.x,k->hit.getY()-4.0f).toInt())!=k.get(),k->element.id+" above");
    }
    for(auto& b:v.buttons)
    {
        probe(v.getComponentAt(b->element.rect.getCentre().toInt())==b.get(),b->element.id+" centre");
        probe(v.getComponentAt(Pt(b->element.rect.getX()-7.0f,b->element.rect.getCentreY()).toInt())!=b.get(),b->element.id+" outside");
    }
    auto& lever=*v.get<studio::Lever>("pattern.mode");
    probe(v.getComponentAt(lever.element.pivot.toInt())==&lever,"pattern.mode pivot");
    probe(v.getComponentAt(v.fold->element.triangle.getCentre().toInt())==v.fold.get(),"pattern.fold triangle");
    probe(v.getComponentAt(v.display->element.glass.getCentre().toInt())==v.display.get(),"pattern.settings glass");
    probe(v.getComponentAt(v.noteDisplay->element.glass.getCentre().toInt())==v.noteDisplay.get(),"pattern.note glass");
    probe(v.getComponentAt(v.roll->element.glass.getCentre().toInt())==v.roll.get(),"pattern.roll glass");
    probe(v.getComponentAt(v.keyboard->element.rect.getCentre().toInt())==v.keyboard.get(),"pattern.keys");
    CHECK(ok==total&&total==42);
    if(!misses.isEmpty())std::cout<<"missed: "<<misses.joinIntoString(", ")<<'\n';
}
void patternRoles()
{
    std::cout<<"SECTION pattern_roles\n";
    PatternRig rig;
    rig.bind("bars",7,3);
    rig.view.roll->show(drawnRoll(rig.model));
    auto* kh=rig.knob("bars").getAccessibilityHandler();
    CHECK(kh!=nullptr&&kh->getRole()==juce::AccessibilityRole::slider&&kh->getValueInterface()!=nullptr
        &&kh->getValueInterface()->getCurrentValueAsString()=="4 BARS");
    auto* rh=rig.view.roll->getAccessibilityHandler();
    CHECK(rh!=nullptr&&rh->getDescription().contains("15 notes"));
    auto* kbh=rig.view.keyboard->getAccessibilityHandler();
    CHECK(kbh!=nullptr&&kbh->getTitle()=="Keyboard");
    auto* ph=rig.view.playing->getAccessibilityHandler();
    CHECK(ph==nullptr||ph->isIgnored());
}
void patternTabOrder()
{
    std::cout<<"SECTION pattern_tab_order\n";
    PatternRig rig;
    for(auto& k:rig.view.knobs)k->setBinding(std::make_unique<Recording>(rig.log,7,3));
    juce::StringArray want;
    for(const auto& id:rig.model.focusOrder.at("pattern"))want.add("pattern."+id);
    juce::StringArray got;
    juce::KeyboardFocusTraverser traverser;
    auto* c=rig.view.control("pattern.fold");
    for(int i=0;i<want.size()&&c!=nullptr;++i){got.add(c->getProperties()["element"].toString());c=traverser.getNextComponent(c);}
    CHECK(want.size()==24&&got==want);
    if(got!=want)std::cout<<"Tab order: "<<got.joinIntoString(", ")<<'\n';
}
void patternTodayIds()
{
    std::cout<<"SECTION pattern_today_ids\n";
    PatternRig rig;
    const std::pair<const char*,const char*> ids[]{{"pattern.mode","pattern.mode"},{"pattern.bars","pattern.bars"},
        {"pattern.meter.numerator","pattern.meter"},{"pattern.grid","pattern.grid"},{"pattern.swing","pattern.swing"},{"pattern.new","pattern.new"},
        {"pattern.load","pattern.load"},{"pattern.save","pattern.save"},{"pattern.browse","pattern.browse"},{"pattern.export","pattern.export"},
        {"pattern.roll","pattern.roll"},{"pattern.note.pitch","pattern.pitch"},{"pattern.note.start","pattern.start"},
        {"pattern.note.duration","pattern.length"},{"pattern.note.velocity","pattern.velocity"},{"pattern.note.channel","pattern.channel"},
        {"pattern.note.add","pattern.add"},{"pattern.note.delete","pattern.delete"},{"pattern.keyboard","pattern.keys"},{"pattern.fold","pattern.fold"},
        {"pattern.playing","pattern.playing"},{"pattern.settings","pattern.settings"},{"pattern.note","pattern.note"},{"pattern.down","pattern.down"},
        {"pattern.up","pattern.up"}};
    juce::StringArray wrong;
    for(const auto& [id,element]:ids)
    {
        auto* c=find(rig,id);
        if(c==nullptr||c->getProperties()["element"].toString()!=element)wrong.add(id);
    }
    CHECK(wrong.isEmpty());
    if(!wrong.isEmpty())std::cout<<"wrong IDs: "<<wrong.joinIntoString(", ")<<'\n';
    for(auto* id:{"pattern.mode","pattern.new","pattern.load","pattern.save","pattern.browse","pattern.export","pattern.note.add",
                  "pattern.note.delete","pattern.down","pattern.up"})
        CHECK(dynamic_cast<juce::Button*>(find(rig,id))!=nullptr);
    for(auto* id:{"pattern.bars","pattern.meter.numerator","pattern.grid","pattern.swing","pattern.note.pitch","pattern.note.start",
                  "pattern.note.duration","pattern.note.velocity","pattern.note.channel"})
    {
        CHECK(dynamic_cast<studio::Knob*>(find(rig,id))!=nullptr);
        CHECK(dynamic_cast<juce::TextEditor*>(find(rig,juce::String(id)+".entry"))!=nullptr);
    }
    CHECK(dynamic_cast<juce::TextEditor*>(find(rig,"pattern.path"))!=nullptr);
    CHECK(dynamic_cast<studio::Roll*>(find(rig,"pattern.roll"))!=nullptr);
    CHECK(dynamic_cast<studio::Keyboard*>(find(rig,"pattern.keyboard"))!=nullptr);
    CHECK(dynamic_cast<studio::Display*>(find(rig,"pattern.note"))!=nullptr&&dynamic_cast<studio::Indicator*>(find(rig,"pattern.playing"))!=nullptr);
    CHECK(find(rig,"pattern.roll.viewport")==nullptr&&find(rig,"pattern.meter.denominator")==nullptr);
}
// The settings display's path entry opens with the shown path, reports a committed one, and Escape keeps the shown one.
void patternPathEntry()
{
    std::cout<<"SECTION pattern_path_entry\n";
    PatternRig rig;
    auto& d=*rig.view.display;
    auto* entry=widget<juce::TextEditor>(rig,"pattern.path");
    if(entry==nullptr)return;
    juce::StringArray committed;
    d.onPath=[&](const juce::String& p){committed.add(p);};
    auto s=drawnStrip();
    s.path="C:\\Patterns\\GROOVE.aipattern";
    d.show(s);
    CHECK(!entry->isVisible()&&entry->getTitle()=="Pattern absolute path");
    d.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(entry->isVisible()&&entry->getText()==s.path);
    entry->setText("  C:\\Patterns\\OTHER.aipattern ");
    entry->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    pump();
    CHECK(!entry->isVisible()&&committed==juce::StringArray{"C:\\Patterns\\OTHER.aipattern"});
    d.mouseDoubleClick(event(d,local(d,d.element.glass.getCentre()),juce::ModifierKeys::leftButtonModifier,2,false));
    CHECK(entry->isVisible()&&entry->getText()==s.path);
    entry->setText("C:\\Elsewhere.aipattern");
    entry->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    pump();
    CHECK(!entry->isVisible()&&entry->getText()==s.path&&committed.size()==1);
    juce::Component::unfocusAllComponents();
}
void knobRules()
{
    std::cout<<"SECTION knob_rules\n";
    PatternRig rig;
    auto& swing=rig.knob("swing");
    CHECK(!swing.isEnabled());
    auto& bars=rig.knob("bars");
    auto& b=rig.bind("bars",7,3);
    auto& log=rig.log;
    using Log=std::vector<juce::String>;
    const auto c=bars.centre;
    CHECK(bars.isEnabled());
    drag(bars,c,c-Pt(0.0f,200.0f/7.0f+2.0f));
    CHECK((log==Log{"begin","set 4","end"}));
    CHECK(std::abs(bars.angle()-(-140.0f+280.0f*4.0f/7.0f))<0.01f);
    log.clear();
    drag(bars,c,c-Pt(0.0f,3.0f*200.0f/7.0f+2.0f));
    CHECK((log==Log{"begin","set 5","set 6","set 7","end"}));
    log.clear();
    b.k=3;
    const auto at=local(bars,c);
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    bars.mouseDown(event(bars,at,held,1,false));
    bars.mouseDrag(event(bars,at-Pt(0.0f,60.0f),held,1,true,at));
    CHECK(bars.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    bars.mouseUp(event(bars,at-Pt(0.0f,60.0f),{},1,true,at));
    CHECK((log==Log{"begin","set 5","cancel"}));
    log.clear();
    b.k=3;
    for(auto code:{juce::KeyPress::upKey,juce::KeyPress::rightKey,juce::KeyPress::downKey,juce::KeyPress::leftKey,juce::KeyPress::pageUpKey,
                   juce::KeyPress::homeKey,juce::KeyPress::endKey,juce::KeyPress::pageDownKey})
        CHECK(bars.keyPressed(juce::KeyPress(code)));
    bars.mouseWheelMove(event(bars,at,{},0,false),wheel(0.5f));
    CHECK((log==Log{"set 4","set 5","set 4","set 3","set 7","set 0","set 7","set 0","set 1"}));
    log.clear();
    bars.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(bars.entry.isVisible()&&bars.entry.getText()=="2");
    CHECK(wait("the knob's entry has the focus",[&]{return bars.entry.hasKeyboardFocus(false);}));
    bars.entry.setText("6",false);
    bars.entry.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    pump();
    CHECK(!bars.entry.isVisible()&&(log==Log{"enter 6"}));
    bars.mouseDoubleClick(event(bars,at,juce::ModifierKeys::leftButtonModifier,2,false));
    CHECK(bars.entry.isVisible());
    bars.entry.setText("2",false);
    bars.entry.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    pump();
    CHECK(!bars.entry.isVisible()&&log.size()==1);
    bars.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(wait("the knob's entry has the focus again",[&]{return bars.entry.hasKeyboardFocus(false);}));
    bars.entry.setText("5",false);
    rig.view.fold->grabKeyboardFocus();
    pump();
    CHECK(!bars.entry.isVisible()&&log.size()==1);
    const auto items=bars.menu();
    CHECK(items.size()==1&&items[0].first=="Denominator 8");
    if(!items.empty())items[0].second();
    CHECK(log.back()=="menu 8");
    auto* h=bars.getAccessibilityHandler();
    if(h!=nullptr&&h->getValueInterface()!=nullptr)
    {
        log.clear();
        b.k=3;
        h->getValueInterface()->setValue(5.0/7.0);
        h->getValueInterface()->setValueAsString("8");
        CHECK((log==Log{"set 5","enter 8"}));
        CHECK(std::abs(h->getValueInterface()->getRange().getInterval()-1.0/7.0)<1e-9);
    }
    log.clear();
    b.on=false;
    bars.refresh();
    CHECK(!bars.isEnabled());
    drag(bars,c,c-Pt(0.0f,100.0f));
    bars.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    bars.mouseWheelMove(event(bars,at,{},0,false),wheel(0.5f));
    bars.mouseDoubleClick(event(bars,at,juce::ModifierKeys::leftButtonModifier,2,false));
    CHECK(log.empty()&&!bars.entry.isVisible());
    juce::Component::unfocusAllComponents();
}
void noteTexts()
{
    std::cout<<"SECTION note_texts\n";
    CHECK(studio::noteName(62)=="D4"&&studio::noteName(60)=="C4"&&studio::noteName(48)=="C3"&&studio::noteName(63)=="D#4"&&studio::noteName(0)=="C-1");
    CHECK(studio::startText(4.5,4,4)=="2.1.3"&&studio::startText(4.75,4,4)=="2.1.4"&&studio::startText(0.0,4,4)=="1.1.1");
    CHECK(studio::startText(4.5+1.0/16.0,4,4)=="2.1.3.2"&&studio::startText(3.0,7,8)=="1.7.1"&&studio::startText(3.5,7,8)=="2.1.1");
    CHECK(studio::lengthText(0.5)=="1/8"&&studio::lengthText(0.75)=="3/16"&&studio::lengthText(4.0)=="1"&&studio::lengthText(0.0625)=="1/64");
    PatternRig rig;
    auto& d=*rig.view.noteDisplay;
    const auto& marks=rig.model.displays.at("pattern.note").spec["marks"];
    auto markLuma=[&](const juce::Image& img)
    {
        double least=1.0e9;
        for(const auto& u:*marks["under"].getArray())
        {
            const auto x=rig.model.element("pattern."+u.toString()).centre.x;
            const auto r=Rect((float)(double)marks["width"],(float)(double)marks["height"]).withCentre({x,(float)(double)marks["y"]+(float)(double)marks["height"]*0.5f});
            least=std::min(least,meanLuma(img,r));
        }
        return least;
    };
    const auto drawn=juce::String("NOTE D4  START 2.1.3  LENGTH 1/8  VELOCITY 96  CHANNEL 1");
    d.show(studio::NoteShown{drawn,drawn,true});
    CHECK(d.placed.count("note")==1&&d.placed.count("empty")==0&&d.placed["note"].shown==drawn&&d.accessibleValue()==drawn);
    const auto lit=markLuma(snap(rig.view));
    d.show(studio::NoteShown{"NO NOTE SELECTED","No note selected",false});
    CHECK(d.placed.count("empty")==1&&d.placed.count("note")==0&&d.placed["empty"].shown=="NO NOTE SELECTED"&&d.accessibleValue()=="No note selected");
    const auto unlit=markLuma(snap(rig.view));
    CHECK(lit>unlit+40.0);
    if(!(lit>unlit+40.0))std::cout<<"marks: lit "<<lit<<", unlit "<<unlit<<'\n';
}
void rollDrawing()
{
    std::cout<<"SECTION roll_drawing\n";
    PatternRig rig;
    auto& roll=*rig.view.roll;
    const auto& rs=rig.model.rolls["pattern"];
    const auto shown=drawnRoll(rig.model);
    roll.show(shown);
    const auto img=snap(rig.view);
    int filled=0,others=0;
    for(int i=0;i<(int)shown.notes.size();++i)
        if(i!=shown.selected)
        {
            ++others;
            const auto r=roll.noteRect(i);
            if(std::abs(meanLuma(img,r.withSizeKeepingCentre(std::max(2.0f,r.getWidth()-6.0f),4.0f))-(double)(int)rs["notes"]["fill"])<25.0)++filled;
        }
    const auto sel=roll.selectedOutline();
    const auto edge=meanLuma(img,Rect(sel.getX()+4.0f,sel.getY(),sel.getWidth()-8.0f,1.0f));
    const auto inside=meanLuma(img,sel.reduced(4.0f,3.5f));
    const auto px=roll.playheadX();
    const auto header=(float)(int)rs["header"]["rule"]["y"]-6.0f;
    const auto playing=meanLuma(img,Rect(px-0.5f,header,1.0f,4.0f));
    auto stopped=shown;
    stopped.playhead.reset();
    roll.show(stopped);
    const auto gone=meanLuma(snap(rig.view),Rect(px-0.5f,header,1.0f,4.0f));
    CHECK(filled==others&&others==14);
    CHECK(edge>200.0&&std::abs(inside-(double)(int)rs["selected"]["fill"])<25.0);
    CHECK(std::abs(px-634.0f)<0.5f&&playing>180.0&&gone<playing-80.0);
    if(!(filled==others&&edge>200.0&&playing>180.0&&gone<playing-80.0))
        std::cout<<filled<<"/"<<others<<" filled; edge "<<edge<<", inside "<<inside<<"; playhead x "<<px<<", "<<playing<<" -> "<<gone<<'\n';
    const auto top=roll.topRow();
    const auto thumb=roll.thumb();
    roll.mouseWheelMove(event(roll,local(roll,roll.element.glass.getCentre()),{},0,false),wheel(0.5f));
    CHECK(top==75&&roll.topRow()==77&&roll.thumb().getY()<thumb.getY());
    CHECK(roll.getDescription()=="Piano roll: 15 notes over 4 bars of 4/4, grid 1/16, note 5 selected");
}
void rollGestures()
{
    std::cout<<"SECTION roll_gestures\n";
    PatternRig rig;
    auto& roll=*rig.view.roll;
    auto shown=drawnRoll(rig.model);
    shown.selected=-1;
    shown.playhead.reset();
    roll.show(shown);
    using Log=std::vector<juce::String>;
    Log log;
    bool admit=true;
    int edits=0;
    roll.onAdmit=[&]{log.push_back("admit");return admit;};
    roll.onSelect=[&](int i){log.push_back("select "+juce::String(i));shown.selected=i;roll.show(shown);};
    roll.onAdd=[&](const studio::Note& n){log.push_back("add");shown.notes.push_back(n);shown.selected=(int)shown.notes.size()-1;roll.show(shown);};
    roll.onEdit=[&](int i,const studio::Note& n){log.push_back("edit "+juce::String(i));++edits;shown.notes[(size_t)i]=n;roll.show(shown);};
    roll.onCancel=[&]{log.push_back("cancel");};
    roll.onDelete=[&]{log.push_back("delete");};
    roll.accepts=[&](int,const studio::Note& n){return n.start>=0.0&&n.start+n.duration<=16.0+1e-9;};
    clickAt(roll,roll.noteRect(0).getCentre());
    CHECK((log==Log{"admit","select 0"}));
    log.clear();
    const auto vel0=shown.notes[0].velocity,ch0=shown.notes[0].channel;
    roll.mouseDoubleClick(event(roll,local(roll,roll.at(2.1,70)),juce::ModifierKeys::leftButtonModifier,2,false));
    CHECK((log==Log{"admit","add"}));
    const auto i=shown.selected;
    CHECK(i==15&&shown.notes[(size_t)i].pitch==70&&std::abs(shown.notes[(size_t)i].start-2.0)<1e-9&&std::abs(shown.notes[(size_t)i].duration-0.25)<1e-9
        &&shown.notes[(size_t)i].velocity==vel0&&shown.notes[(size_t)i].channel==ch0);
    log.clear();
    const auto from=roll.noteRect(i).getCentre();
    drag(roll,from,from+Pt(roll.beatWidth()*0.25f,-roll.rowHeight()));
    CHECK((log==Log{"admit","select 15","edit 15"}));
    CHECK(std::abs(shown.notes[(size_t)i].start-2.25)<1e-9&&shown.notes[(size_t)i].pitch==71);
    const auto r=roll.noteRect(i);
    drag(roll,Pt(r.getRight()-1.0f,r.getCentreY()),Pt(r.getRight()-1.0f+roll.beatWidth()*0.25f,r.getCentreY()));
    CHECK(std::abs(shown.notes[(size_t)i].duration-0.5)<1e-9&&std::abs(shown.notes[(size_t)i].start-2.25)<1e-9);
    const auto v=shown.notes[(size_t)i].velocity;
    drag(roll,roll.noteRect(i).getCentre(),roll.noteRect(i).getCentre()-Pt(0.0f,10.0f),juce::ModifierKeys::altModifier);
    CHECK(shown.notes[(size_t)i].velocity==juce::jlimit(1,127,v+10)&&shown.notes[(size_t)i].pitch==71&&edits==3);
    log.clear();
    const auto kept=shown.notes[(size_t)i];
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    const auto c=local(roll,roll.noteRect(i).getCentre());
    roll.mouseDown(event(roll,c,held,1,false));
    roll.mouseDrag(event(roll,c+Pt(60.0f,0.0f),held,1,true,c));
    roll.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    roll.mouseUp(event(roll,c+Pt(60.0f,0.0f),{},1,true,c));
    CHECK((log==Log{"admit","select 15","cancel"})&&shown.notes[(size_t)i].start==kept.start&&edits==3);
    roll.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    roll.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey));
    CHECK(log.size()==5&&log[3]=="delete"&&log[4]=="delete");
    log.clear();
    admit=false;
    clickAt(roll,roll.noteRect(1).getCentre());
    roll.mouseDoubleClick(event(roll,local(roll,roll.at(3.1,70)),juce::ModifierKeys::leftButtonModifier,2,false));
    CHECK((log==Log{"admit","admit"}));
    admit=true;
    log.clear();
    roll.scrollTo(60);
    CHECK(roll.topRow()==60);
    const auto hidden=roll.noteRect(0).getCentre();
    const auto row50=roll.at(0.0,50);
    clickAt(roll,Pt(row50.x-10.0f,row50.y));
    clickAt(roll,Pt(roll.at(1.0,50).x,(float)(int)rig.model.rolls["pattern"]["grid"]["top"]-10.0f));
    clickAt(roll,hidden);
    drag(roll,hidden,hidden+Pt(40.0f,40.0f));
    roll.mouseDoubleClick(event(roll,local(roll,hidden),juce::ModifierKeys::leftButtonModifier,2,false));
    CHECK(std::all_of(log.begin(),log.end(),[](const juce::String& s){return s=="admit";})&&edits==3);
    roll.reveal(70);
    CHECK(roll.topRow()==70);
    roll.reveal(30);
    CHECK(roll.topRow()==30+roll.rowsShown()-1);
}
void keyboardRules()
{
    std::cout<<"SECTION keyboard_rules\n";
    PatternRig rig;
    auto& kb=*rig.view.keyboard;
    using Log=std::vector<juce::String>;
    Log log;
    bool accept=true;
    kb.onKey=[&](bool on,int pitch,int velocity)
    {
        if(!accept)return false;
        log.push_back(on?"on "+juce::String(pitch)+" "+juce::String(velocity):"off "+juce::String(pitch));
        return true;
    };
    const auto c3=kb.keyRect(0),cs3=kb.keyRect(1);
    CHECK(kb.keyCount()==25&&kb.lowestKey()==48&&cs3.getHeight()<c3.getHeight()&&cs3.getWidth()<c3.getWidth()&&kb.pitchAt(low(c3))==48&&kb.pitchAt(cs3.getCentre())==49);
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    const auto at=local(kb,low(c3));
    kb.mouseDown(event(kb,at,held,1,false));
    const auto playing=kb.isPlaying();
    kb.mouseUp(event(kb,at,{},1,false));
    CHECK(playing&&!kb.isPlaying()&&(log==Log{"on 48 100","off 48"}));
    log.clear();
    drag(kb,low(kb.keyRect(0)),low(kb.keyRect(4)));
    CHECK((log==Log{"on 48 100","off 48","on 50 100","off 50","on 52 100","off 52"}));
    kb.shift(1);
    const auto up1=kb.lowestKey();
    for(int i=0;i<5;++i)kb.shift(1);
    const auto top=kb.lowestKey();
    for(int i=0;i<10;++i)kb.shift(-1);
    CHECK(up1==60&&top==96&&kb.lowestKey()==0);
    for(int i=0;i<4;++i)kb.shift(1);
    CHECK(kb.lowestKey()==48);
    std::set<int> down;
    kb.isKeyDown=[&](const juce::KeyPress& k){return down.count(juce::CharacterFunctions::toLowerCase((juce::juce_wchar)k.getKeyCode()))>0;};
    kb.grabKeyboardFocus();
    CHECK(wait("the keyboard has the focus",[&]{return kb.hasKeyboardFocus(false);}));
    log.clear();
    CHECK(kb.keyPressed(juce::KeyPress('a',{},0))&&!kb.keyPressed(juce::KeyPress('z',{},0)));
    down.insert('a');
    CHECK(kb.keyStateChanged(true)&&kb.isPlaying());
    down.erase('a');
    kb.keyStateChanged(false);
    down.insert('w');
    kb.keyStateChanged(true);
    rig.view.fold->grabKeyboardFocus();
    pump();
    CHECK((log==Log{"on 72 127","off 72","on 73 127","off 73"})&&!kb.isPlaying());
    log.clear();
    kb.mouseDown(event(kb,at,held,1,false));
    kb.releaseAll();
    kb.mouseUp(event(kb,at,{},1,false));
    CHECK((log==Log{"on 48 100","off 48"})&&!kb.isPlaying());
    log.clear();
    kb.mouseDown(event(kb,at,held,1,false));
    kb.forget();
    kb.mouseUp(event(kb,at,{},1,false));
    CHECK((log==Log{"on 48 100"})&&!kb.isPlaying());
    log.clear();
    accept=false;
    kb.mouseDown(event(kb,at,held,1,false));
    const auto refusedPlays=kb.isPlaying();
    accept=true;
    kb.mouseUp(event(kb,at,{},1,false));
    CHECK(!refusedPlays&&log.empty());
    juce::Component::unfocusAllComponents();
}
// The unfolded unit's drawn pattern (elements.json's mockup.pattern) as a processor pattern.
PatternDocument drawnPattern(const studio::Model& m)
{
    const auto shown=drawnRoll(m);
    auto d=settings((std::uint8_t)shown.bars,{(std::uint8_t)shown.numerator,(std::uint8_t)shown.denominator},PatternGrid::sixteenth,0.0);
    d.noteCount=(std::uint16_t)shown.notes.size();
    for(std::size_t i=0;i<shown.notes.size();++i)
    {
        const auto& n=shown.notes[i];
        d.notes[i]={n.start,n.duration,(std::uint8_t)n.pitch,(std::uint8_t)n.velocity,(std::uint8_t)n.channel};
    }
    return d;
}
// The unfolded PATTERN unit on the desktop over a prepared processor holding the drawn pattern, INTERNAL unless told
// otherwise, its host stopped.
struct UnitRig
{
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    FakeServices services;
    Playhead head;
    const juce::File dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("studio-unit-"+juce::Uuid().toString());
    std::unique_ptr<studio::PatternUnit> unit;
    explicit UnitRig(PlaybackMode mode=PlaybackMode::internalPattern)
    {
        dir.createDirectory();
        CHECK(copperArcInstance(p));p.setPlayHead(&head);p.prepareToPlay(48000,64);
        std::uint64_t e=0;project(&e);CHECK(p.applyPatternEdit(drawnPattern(model()),e));
        project(&e);CHECK(p.setPlaybackMode(mode,e));
        block();
        unit=std::make_unique<studio::PatternUnit>(p,model(),services.make());
        unit->addToDesktop(0);unit->setVisible(true);
    }
    ~UnitRig() { unit.reset();p.setPlayHead(nullptr);p.releaseResources();dir.deleteRecursively(); }
    const studio::Model& model() const { return *studio::studioSkin(); }
    void block() { juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;p.processBlock(audio,midi); }
    ProjectDocument project(std::uint64_t* revision=nullptr)
    { ProjectDocument d;std::uint64_t e=0,r=0;CHECK(p.tryProjectSnapshot(d,e,r));if(revision)*revision=e;return d; }
    std::uint64_t revision() { std::uint64_t e=0;project(&e);return e; }
    // A host recall of the current project with note 0 an octave lower.
    ProjectDocument recall()
    {
        auto d=project();d.pattern.notes[0].pitch=(std::uint8_t)(d.pattern.notes[0].pitch-12);
        juce::MemoryBlock b;CHECK(encodeProjectState(d,b));CHECK(p.restoreProjectState(b.getData(),(int)b.getSize()));
        return d;
    }
    template<class T> T& get(const char* id) { auto* c=widget<T>(*unit,id);if(c==nullptr)throw std::runtime_error(id);return *c; }
    studio::Knob& knob(const char* id) { return get<studio::Knob>(id); }
    studio::Roll& roll() { return get<studio::Roll>("pattern.roll"); }
    studio::Keyboard& keyboard() { return get<studio::Keyboard>("pattern.keyboard"); }
    studio::Display& settingsDisplay() { return get<studio::Display>("pattern.settings"); }
    juce::String settingsLine() { unit->refresh();return settingsDisplay().strip.text; }
    juce::String noteLine() { unit->refresh();return get<studio::Display>("pattern.note").note.text; }
    juce::String status() { return get<juce::Label>("pattern.status").getText(); }
    void select(int i) { clickAt(roll(),roll().noteRect(i).getCentre());CHECK(roll().shown().selected==i); }
    void path(const juce::String& text) { get<juce::TextEditor>("pattern.path").setText(text);pump(); }
};
void patternSettingsKnobs()
{
    std::cout<<"SECTION pattern_settings_knobs\n";
    UnitRig rig;
    auto& bars=rig.knob("pattern.bars");
    const auto before=rig.revision();
    drag(bars,bars.centre,bars.centre-Pt(0.0f,200.0f/7.0f+2.0f));
    CHECK(rig.project().pattern.bars==5&&rig.revision()==before+1&&rig.status().startsWith("Pattern settings complete"));
    CHECK(std::abs(bars.angle()-20.0f)<0.01f&&rig.settingsLine()=="5 BARS  4/4  1/16  SWING 0%");
    bars.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
    bars.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
    CHECK(rig.project().pattern.bars==4&&rig.status().startsWith("Invalid phrase size: edit notes to fit before shrinking bars or meter"));
    CHECK(rig.settingsLine()=="NOTES PAST THE END: MOVE THEM FIRST"&&rig.settingsDisplay().strip.amber);
    auto& grid=rig.knob("pattern.grid");
    grid.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    const auto up=rig.project().pattern.grid;
    grid.keyPressed(juce::KeyPress(juce::KeyPress::homeKey));
    const auto home=rig.project().pattern.grid;
    grid.keyPressed(juce::KeyPress(juce::KeyPress::endKey));
    CHECK(up==PatternGrid::thirtySecond&&home==PatternGrid::quarter&&rig.project().pattern.grid==PatternGrid::sixtyFourth);
    auto& swing=rig.knob("pattern.swing");
    {const auto held=rig.project().pattern.swing;studio::noteRackScroll();swing.mouseWheelMove(event(swing,local(swing,swing.centre),{},0,false),wheel(0.5f));CHECK(rig.project().pattern.swing==held);juce::Thread::sleep(450);}
    swing.mouseWheelMove(event(swing,local(swing,swing.centre),{},0,false),wheel(0.5f));
    const auto one=rig.project().pattern.swing;
    swing.keyPressed(juce::KeyPress(juce::KeyPress::pageUpKey));
    CHECK(std::abs(one-0.01)<1e-9&&std::abs(rig.project().pattern.swing-0.11)<1e-9);
    auto& meter=rig.knob("pattern.meter.numerator");
    juce::StringArray items;
    for(const auto& i:meter.menu())items.add(i.first);
    CHECK(items==juce::StringArray({"Denominator 1","Denominator 2","Denominator 4","Denominator 8","Denominator 16","Denominator 32"}));
    auto choose=[&](const juce::String& name){for(const auto& i:meter.menu())if(i.first==name)i.second();};
    choose("Denominator 8");
    CHECK(rig.project().pattern.meter.denominator==4&&rig.status().startsWith("Invalid phrase size"));
    choose("Denominator 2");
    CHECK(rig.project().pattern.meter.denominator==2&&rig.settingsLine().contains("4/2"));
}
void patternNoteKnobs()
{
    std::cout<<"SECTION pattern_note_knobs\n";
    UnitRig rig;
    const auto noteKnobs={"pattern.note.pitch","pattern.note.start","pattern.note.duration","pattern.note.velocity","pattern.note.channel"};
    auto disabled=[&]
    {
        bool all=!rig.get<studio::SkinButton>("pattern.note.delete").isEnabled();
        for(auto* id:noteKnobs)all&=!rig.knob(id).isEnabled();
        return all;
    };
    CHECK(rig.noteLine()=="NO NOTE SELECTED"&&disabled()&&rig.knob("pattern.bars").isEnabled());
    rig.select(4);
    CHECK(rig.noteLine()=="NOTE D4  START 2.1.3  LENGTH 1/8  VELOCITY 96  CHANNEL 1"&&rig.knob("pattern.note.pitch").isEnabled());
    int edits=0;
    for(auto [id,key]:{std::pair{"pattern.note.pitch",juce::KeyPress::upKey},std::pair{"pattern.note.start",juce::KeyPress::upKey},
                       std::pair{"pattern.note.duration",juce::KeyPress::upKey},std::pair{"pattern.note.velocity",juce::KeyPress::pageUpKey},
                       std::pair{"pattern.note.channel",juce::KeyPress::endKey}})
    {
        const auto before=rig.revision();
        rig.knob(id).keyPressed(juce::KeyPress(key));
        edits+=rig.revision()==before+1?1:0;
    }
    const auto n=rig.project().pattern.notes[4];
    CHECK(n.pitch==63&&std::abs(n.startBeats-4.75)<1e-9&&std::abs(n.durationBeats-0.75)<1e-9&&n.velocity==106&&n.channel==16);
    CHECK(edits==5&&rig.status().startsWith("Note value complete"));
    CHECK(rig.noteLine()=="NOTE D#4  START 2.1.4  LENGTH 3/16  VELOCITY 106  CHANNEL 16");
    click(*rig.unit,"pattern.note.add");
    const auto added=rig.project().pattern;
    CHECK(added.noteCount==16&&(added.notes[15]==PatternNote{0.0,0.25,60,100,1})&&rig.roll().shown().selected==15&&rig.status().startsWith("Add note complete"));
    click(*rig.unit,"pattern.note.delete");
    CHECK(rig.project().pattern.noteCount==15&&rig.status().startsWith("Delete note complete"));
    auto full=rig.project().pattern;
    for(std::size_t i=full.noteCount;i<maximumPatternNotes;++i)full.notes[i]={0.0,0.25,(std::uint8_t)(30+i%60),100,1};
    full.noteCount=(std::uint16_t)maximumPatternNotes;
    std::uint64_t e=0;rig.project(&e);CHECK(rig.p.applyPatternEdit(full,e));rig.unit->refresh();
    click(*rig.unit,"pattern.note.add");
    CHECK(rig.project().pattern.noteCount==256&&rig.status().startsWith("Pattern is full (256 notes); delete a note before adding"));
    CHECK(rig.settingsLine()=="PATTERN FULL: 256 NOTES");
}
void patternRoll()
{
    std::cout<<"SECTION pattern_roll\n";
    UnitRig rig;
    auto& roll=rig.roll();
    rig.select(0);
    const auto vel0=rig.project().pattern.notes[0].velocity;
    roll.mouseDoubleClick(event(roll,local(roll,roll.at(2.1,70)),juce::ModifierKeys::leftButtonModifier,2,false));
    const auto added=rig.project().pattern;
    CHECK(added.noteCount==16&&(added.notes[15]==PatternNote{2.0,0.25,70,vel0,1})&&roll.shown().selected==15&&rig.status().startsWith("Add note complete"));
    const auto before=rig.revision();
    const auto from=roll.noteRect(15).getCentre();
    drag(roll,from,from+Pt(roll.beatWidth()*0.25f,-roll.rowHeight()));
    auto n=rig.project().pattern.notes[15];
    const auto moved=std::abs(n.startBeats-2.25)<1e-9&&n.pitch==71&&rig.status().startsWith("Note edit complete");
    const auto r=roll.noteRect(15);
    drag(roll,Pt(r.getRight()-1.0f,r.getCentreY()),Pt(r.getRight()-1.0f+roll.beatWidth()*0.25f,r.getCentreY()));
    n=rig.project().pattern.notes[15];
    const auto resized=std::abs(n.durationBeats-0.5)<1e-9&&std::abs(n.startBeats-2.25)<1e-9;
    const auto v=n.velocity;
    drag(roll,roll.noteRect(15).getCentre(),roll.noteRect(15).getCentre()-Pt(0.0f,10.0f),juce::ModifierKeys::altModifier);
    n=rig.project().pattern.notes[15];
    CHECK(moved&&resized&&n.velocity==juce::jlimit(1,127,v+10)&&n.pitch==71&&rig.revision()==before+3);
    const auto kept=rig.project();
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    const auto c=local(roll,roll.noteRect(15).getCentre());
    roll.mouseDown(event(roll,c,held,1,false));
    roll.mouseDrag(event(roll,c+Pt(60.0f,0.0f),held,1,true,c));
    roll.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    roll.mouseUp(event(roll,c+Pt(60.0f,0.0f),{},1,true,c));
    CHECK(rig.project()==kept&&rig.status().startsWith("Edit cancelled; pattern preserved"));
    roll.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    CHECK(rig.project().pattern.noteCount==15&&rig.status().startsWith("Delete note complete"));
}
void patternRollDrawing()
{
    std::cout<<"SECTION pattern_roll_drawing\n";
    UnitRig rig;
    const auto& rs=rig.model().rolls["pattern"];
    rig.head.playing=true;rig.head.bpm=120.0;rig.head.ppq=(double)rig.model().patternMockup["playhead_beats"];
    rig.block();rig.unit->refresh();
    auto& roll=rig.roll();
    const auto px=roll.playheadX();
    const auto header=(float)(int)rs["header"]["rule"]["y"]-6.0f;
    const auto playing=meanLuma(snap(*rig.unit),Rect(px-0.5f,header,1.0f,4.0f));
    CHECK(roll.shown().playhead.has_value()&&std::abs(px-634.0f)<0.5f&&playing>180.0);
    rig.head.playing=false;rig.block();rig.unit->refresh();
    const auto stopped=meanLuma(snap(*rig.unit),Rect(px-0.5f,header,1.0f,4.0f));
    CHECK(!roll.shown().playhead.has_value()&&stopped<playing-80.0);
}
void patternKeyboard()
{
    std::cout<<"SECTION pattern_keyboard\n";
    {
        UnitRig rig(PlaybackMode::externalMidi);
        auto& kb=rig.keyboard();
        auto& play=rig.get<studio::SkinButton>("pattern.down");
        auto& available=rig.get<studio::SkinButton>("pattern.up");
        const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto at=local(kb,low(kb.keyRect(0)));
        CHECK(available.lit()&&!play.lit());
        kb.mouseDown(event(kb,at,held,1,false));
        juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;
        rig.p.processBlock(audio,midi);
        CHECK(play.lit()&&kb.isHeld(48)&&audio.getMagnitude(0,64)>0.0f);
        kb.mouseUp(event(kb,at,{},1,false));
        CHECK(!play.lit()&&!kb.isPlaying());
        rig.block();
        bool queued=true;
        for(int i=0;i<256;++i)queued&=rig.p.enqueueKeyboardNote(true,60,100,1);
        kb.mouseDown(event(kb,at,held,1,false));
        CHECK(queued&&!kb.isPlaying()&&!play.lit()&&rig.status().startsWith("Keyboard queue full; notes released. Try again."));
        CHECK(rig.settingsLine()=="KEYS FULL: NOTES RELEASED");
        kb.mouseUp(event(kb,at,{},1,false));
    }
    {
        UnitRig rig;
        rig.head.playing=true;rig.head.bpm=120.0;rig.head.ppq=0.0;
        rig.block();rig.unit->refresh();
        CHECK(!rig.get<studio::SkinButton>("pattern.up").lit());
    }
}
void patternPlayingLed()
{
    std::cout<<"SECTION pattern_playing_led\n";
    UnitRig rig;
    rig.head.playing=true;rig.head.bpm=120.0;rig.head.ppq=0.0;
    rig.block();rig.unit->refresh();
    auto& playing=rig.get<studio::Indicator>("pattern.playing");
    auto& lever=rig.get<studio::Lever>("pattern.mode");
    const auto lit=playing.isLit()&&lever.position()==1;
    const auto before=rig.revision();
    lever.set(0);
    CHECK(lit&&!playing.isLit()&&rig.project().mode==PlaybackMode::externalMidi&&rig.revision()==before+1&&rig.status().startsWith("Mode edit complete"));
}
void patternKnobGestures()
{
    std::cout<<"SECTION pattern_knob_gestures\n";
    UnitRig rig;
    auto& bars=rig.knob("pattern.bars");
    const auto before=rig.revision();
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    const auto from=local(bars,bars.centre);
    auto above=[&](int detents){return from-Pt(0.0f,(float)detents*200.0f/7.0f+2.0f);};
    bars.mouseDown(event(bars,from,held,1,false));
    juce::StringArray previews;
    for(int i=1;i<=3;++i)
    {
        bars.mouseDrag(event(bars,above(i),held,1,true,from));
        previews.add(rig.settingsLine()+"|"+juce::String(rig.roll().shown().bars)+"|"+juce::String(rig.project().pattern.bars));
    }
    bars.mouseUp(event(bars,above(3),{},1,true,from));
    CHECK(previews==juce::StringArray({"5 BARS  4/4  1/16  SWING 0%|5|4","6 BARS  4/4  1/16  SWING 0%|6|4","7 BARS  4/4  1/16  SWING 0%|7|4"}));
    CHECK(rig.project().pattern.bars==7&&rig.revision()==before+1&&rig.status().startsWith("Pattern settings complete"));
    CHECK(rig.p.undoSoundEdit()&&rig.project().pattern.bars==4);
    rig.unit->refresh();
    const auto kept=rig.project();
    bars.mouseDown(event(bars,from,held,1,false));
    bars.mouseDrag(event(bars,above(1),held,1,true,from));
    const auto previewed=rig.settingsLine();
    bars.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    bars.mouseUp(event(bars,above(1),{},1,true,from));
    const auto cancelled=rig.project()==kept&&rig.status().startsWith("Edit cancelled; pattern preserved");
    rig.services.now+=5000;
    CHECK(previewed.startsWith("5 BARS")&&cancelled&&rig.settingsLine().startsWith("4 BARS"));
}
void patternRecallDuringGestures()
{
    std::cout<<"SECTION pattern_recall_during_gestures\n";
    const juce::String cancelled="Edit cancelled: project changed; select the note again";
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    // Without the editor's timer between the recall and the release, and with it.
    for(bool timer:{false,true})
    {
        UnitRig rig;
        auto& bars=rig.knob("pattern.bars");
        const auto from=local(bars,bars.centre),to=from-Pt(0.0f,200.0f/7.0f+2.0f);
        bars.mouseDown(event(bars,from,held,1,false));
        bars.mouseDrag(event(bars,to,held,1,true,from));
        const auto first=rig.recall();
        if(timer)rig.unit->refresh();
        bars.mouseUp(event(bars,to,{},1,true,from));
        CHECK(rig.project()==first&&rig.status().startsWith(cancelled)&&!bars.isDragging());
        auto& roll=rig.roll();
        const auto c=local(roll,roll.noteRect(1).getCentre());
        roll.mouseDown(event(roll,c,held,1,false));
        roll.mouseDrag(event(roll,c+Pt(roll.beatWidth(),0.0f),held,1,true,c));
        const auto second=rig.recall();
        if(timer)rig.unit->refresh();
        roll.mouseUp(event(roll,c+Pt(roll.beatWidth(),0.0f),{},1,true,c));
        CHECK(rig.project()==second&&rig.status().startsWith(cancelled)&&!roll.isDragging());
        rig.select(4);
        auto& pitch=rig.knob("pattern.note.pitch");
        pitch.openEntry();
        pitch.entry.setText("70",false);
        const auto third=rig.recall();
        if(timer)rig.unit->refresh();
        CHECK(pitch.entry.isVisible()!=timer);
        pitch.entry.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        pump();
        CHECK(rig.project()==third&&rig.status().startsWith(cancelled)&&!pitch.entry.isVisible());
    }
}
void patternExactEntry()
{
    std::cout<<"SECTION pattern_exact_entry\n";
    UnitRig rig;
    auto type=[](studio::Knob& k,const juce::String& text)
    { k.openEntry();k.entry.setText(text,false);k.entry.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));pump(); };
    rig.select(4);
    auto& start=rig.knob("pattern.note.start");
    start.openEntry();
    const auto opened=start.entry.getText();
    start.closeEntry();
    type(start,"0.12345678901234566");
    const auto exact=rig.project().pattern.notes[4].startBeats==0.12345678901234566&&rig.status().startsWith("Note value complete");
    start.openEntry();
    CHECK(opened=="4.5"&&exact&&start.entry.getText()=="0.12345678901234566");
    start.closeEntry();
    for(const char* bad:{".5trailing","nan","inf","","1 2"})
    {
        const auto kept=rig.project();
        type(start,bad);
        CHECK(rig.project()==kept&&rig.status().startsWith("Invalid note value: use a finite number within the phrase and MIDI range"));
    }
    type(rig.knob("pattern.bars"),"8");
    type(rig.knob("pattern.meter.numerator"),"7/8");
    const auto d=rig.project().pattern;
    CHECK(d.bars==8&&(d.meter==PhraseMeter{7,8})&&rig.status().startsWith("Pattern settings complete"));
    type(rig.knob("pattern.grid"),"1/64");
    CHECK(rig.project().pattern.grid==PatternGrid::sixtyFourth);
    const auto kept=rig.project();
    type(rig.knob("pattern.grid"),"1/5");
    CHECK(rig.project()==kept&&rig.status().startsWith("Invalid settings: bars 1-8, meter 1-32, swing 0-1"));
}
void patternFiles()
{
    std::cout<<"SECTION pattern_files\n";
    UnitRig rig;
    const auto saved=rig.dir.getChildFile("drawn.aipattern"),mid=rig.dir.getChildFile("drawn.mid");
    rig.path(saved.getFullPathName());
    click(*rig.unit,"pattern.save");
    PatternDocument loaded;
    CHECK(loadPatternAsset(saved,loaded)&&loaded==rig.project().pattern&&rig.status().startsWith("Save pattern complete"));
    CHECK(rig.settingsLine()=="PATTERN SAVED");
    rig.select(4);
    rig.knob("pattern.note.pitch").keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(rig.project().pattern!=loaded);
    click(*rig.unit,"pattern.load");
    CHECK(rig.project().pattern==loaded&&rig.roll().shown().selected==-1&&rig.status().startsWith("Load pattern complete"));
    rig.path(mid.getFullPathName());
    click(*rig.unit,"pattern.export");
    CHECK(mid.existsAsFile()&&rig.status().startsWith("MIDI export complete"));
    rig.path("drawn.aipattern");
    click(*rig.unit,"pattern.save");
    CHECK(rig.status().startsWith("Pattern operation failed: enter an absolute file path"));
    rig.path("");
    click(*rig.unit,"pattern.save");
    const auto& requests=rig.services.requests;
    CHECK(requests.size()==1&&requests[0].title=="Pattern Save"&&requests[0].patterns=="*.aipattern"&&requests[0].saving
          &&requests[0].osDialog&&requests[0].parent==rig.unit.get());
    CHECK(rig.status().startsWith("Pattern selection cancelled; committed project preserved"));
    rig.services.answer=saved;
    click(*rig.unit,"pattern.browse");
    auto& display=rig.settingsDisplay();
    display.openEntry();
    const auto reopened=rig.get<juce::TextEditor>("pattern.path").getText();
    display.closeEntry();
    CHECK(requests.size()==2&&requests[1].title=="Pattern Browse"&&!requests[1].saving&&rig.status().startsWith("Pattern path selected")
          &&reopened==saved.getFullPathName());
    rig.path("");
    rig.services.hold=true;
    click(*rig.unit,"pattern.export");
    click(*rig.unit,"pattern.load");
    CHECK(requests.size()==3&&requests[2].title=="Pattern Export"&&requests[2].patterns=="*.mid"
          &&rig.status().startsWith("Finish or cancel the open pattern chooser first"));
    if(rig.services.held)rig.services.held(juce::File());
    CHECK(rig.status().startsWith("Pattern selection cancelled; committed project preserved"));
}
// The unfolded unit's EXPORT drags the committed pattern out as the strip's does, and its settings display says so.
void patternExportDrag()
{
    std::cout<<"SECTION pattern_export_drag\n";
    UnitRig rig;
    const auto temp=rig.dir.getChildFile("temp");
    CHECK(temp.createDirectory());
    TempFolder redirect(temp);
    const auto store=temp.getChildFile("AI Rack Synth MIDI");
    auto& b=rig.get<studio::SkinButton>("pattern.export");
    CHECK(b.getTooltip()=="Drag MIDI into your DAW; click or press Return to save a MIDI file");
    const auto at=b.element.rect.getCentre();
    const auto before=rig.project();
    drag(b,at,at+Pt(12,0));
    const auto exported=store.findChildFiles(juce::File::findFiles,false,"airack-*.mid");
    CHECK(exported.size()==1&&rig.services.drags.size()==1&&rig.services.requests.empty()&&rig.project()==before);
    if(exported.size()==1)CHECK(midiNotes(exported[0])==(int)before.pattern.noteCount&&importerReads(exported[0]));
    if(!rig.services.drags.empty())CHECK(rig.services.drags.back().source==&b&&!rig.services.drags.back().canMove);
    CHECK(rig.status().startsWith("MIDI file ready: drop into your DAW to copy the pattern")&&rig.settingsLine()=="DROP MIDI IN YOUR DAW");
    drag(b,at,at+Pt(12,0));
    CHECK(rig.services.drags.size()==1&&rig.settingsLine()=="MIDI DRAG IN PROGRESS");
    rig.unit.reset();
    rig.services.endDrag();
    pump();
    CHECK(exported.size()==1&&exported[0].existsAsFile());
}
void patternChooserOutlivesUnit()
{
    std::cout<<"SECTION pattern_chooser_outlives_unit\n";
    UnitRig rig;
    rig.services.hold=true;
    click(*rig.unit,"pattern.save");
    CHECK(rig.services.held!=nullptr);
    rig.unit.reset();
    const auto late=rig.dir.getChildFile("late.aipattern");
    if(rig.services.held)rig.services.held(late);
    CHECK(!late.existsAsFile());
}
// A rendering against the prototype's native snapshot of the same state: at most 1 level of mean absolute difference
// per channel, and no 32 x 32 tile above 8 in its worst channel. A failure prints the worst tiles. The snapshot's
// `unmatched` regions show what the studio no longer draws, and are left out.
void matchesPrototype(const juce::Image& rendered,const juce::Image& want,const char* what,std::initializer_list<juce::Rectangle<int>> unmatched={})
{
    CHECK(rendered.getWidth()==want.getWidth()&&rendered.getHeight()==want.getHeight());
    if(rendered.getWidth()!=want.getWidth()||rendered.getHeight()!=want.getHeight())
    {
        std::cout<<"  "<<what<<": rendered "<<rendered.getWidth()<<" x "<<rendered.getHeight()<<", drawn "<<want.getWidth()<<" x "<<want.getHeight()<<'\n';
        return;
    }
    auto got=rendered.createCopy();
    for(const auto& r:unmatched)
        for(int y=r.getY();y<r.getBottom();++y)
            for(int x=r.getX();x<r.getRight();++x)got.setPixelAt(x,y,want.getPixelAt(x,y));
    const juce::Image::BitmapData a(got,juce::Image::BitmapData::readOnly),b(want,juce::Image::BitmapData::readOnly);
    struct Tile { int x,y;double diff; };
    std::vector<Tile> tiles;
    std::array<double,3> sum{};
    for(int ty=0;ty<got.getHeight();ty+=32)
        for(int tx=0;tx<got.getWidth();tx+=32)
        {
            std::array<double,3> tile{};
            int n=0;
            for(int y=ty;y<std::min(ty+32,got.getHeight());++y)
                for(int x=tx;x<std::min(tx+32,got.getWidth());++x,++n)
                {
                    const auto p=a.getPixelColour(x,y),q=b.getPixelColour(x,y);
                    const std::array<double,3> d{std::abs((double)p.getRed()-q.getRed()),std::abs((double)p.getGreen()-q.getGreen()),std::abs((double)p.getBlue()-q.getBlue())};
                    for(int c=0;c<3;++c){tile[c]+=d[c];sum[c]+=d[c];}
                }
            tiles.push_back({tx,ty,std::max({tile[0],tile[1],tile[2]})/n});
        }
    const auto mean=std::max({sum[0],sum[1],sum[2]})/((double)got.getWidth()*got.getHeight());
    std::sort(tiles.begin(),tiles.end(),[](const Tile& l,const Tile& r){return l.diff>r.diff;});
    const bool ok=mean<=1.0&&tiles.front().diff<=8.0;
    CHECK(ok);
    std::cout<<"  "<<what<<": mean "<<mean<<", worst tile "<<tiles.front().diff<<'\n';
    if(!ok)
        for(std::size_t i=0;i<std::min<std::size_t>(10,tiles.size());++i)
            std::cout<<"    tile "<<tiles[i].x<<","<<tiles[i].y<<": "<<tiles[i].diff<<'\n';
}
// The unit over the real processor in the prototype's two drawn states, rendered at 1x, against its native snapshots.
void patternMatchesPrototype()
{
    std::cout<<"SECTION pattern_matches_prototype\n";
    const auto native=juce::File(RACKSYNTH_WORKSPACE).getChildFile("docs/design/racks/2026-10-09-studio-rack/window/skin/compare/native");
    // A fractional desktop scale can round the unit's window up a pixel; the drawn unit stays 1547 x 702.
    const auto drawn=[](studio::PatternUnit& unit){return unit.createComponentSnapshot({1547,702},true,1.0f);};
    {
        UnitRig rig;
        rig.head.playing=true;rig.head.bpm=120.0;rig.head.ppq=(double)rig.model().patternMockup["playhead_beats"];
        rig.block();
        rig.unit->refresh();
        rig.select((int)rig.model().patternMockup["selected"]);
        juce::Component::unfocusAllComponents();
        rig.unit->refresh();
        CHECK(rig.roll().shown().playhead.has_value()&&rig.noteLine()=="NOTE D4  START 2.1.3  LENGTH 1/8  VELOCITY 96  CHANNEL 1");
        const auto stage=juce::ImageFileFormat::loadFrom(native.getChildFile("stage-pattern.png"));
        CHECK(stage.getWidth()==1547&&stage.getHeight()==965);
        if(stage.isValid())matchesPrototype(drawn(*rig.unit),stage.getClippedImage({0,263,1547,702}),"drawn state");
    }
    {
        UnitRig rig(PlaybackMode::externalMidi);
        rig.knob("pattern.bars").keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        juce::Component::unfocusAllComponents();
        CHECK(rig.status().startsWith("Invalid phrase size: edit notes to fit before shrinking bars or meter"));
        CHECK(rig.settingsLine()=="NOTES PAST THE END: MOVE THEM FIRST"&&!rig.roll().shown().playhead.has_value());
        const auto states=juce::ImageFileFormat::loadFrom(native.getChildFile("pattern-states.png"));
        CHECK(states.getWidth()==1547&&states.getHeight()==702);
        if(states.isValid())matchesPrototype(drawn(*rig.unit),states,"second state");
    }
}
// An example rack placed with an explicit allocation (local unit, zero-based physical bank). With `housePlate`, a copy of
// the example under another design ID, which keeps its name and the house plate: every example now has a faceplate, so
// the checks of the house plate's own parts place such a copy.
juce::Uuid placeRack(InstrumentProcessor& p,std::size_t example,RackDestination destination,std::vector<UnitBinding> allocation,bool reuse=false,
                     bool housePlate=false)
{
    RackOperation op;op.design=airack::test::frozenExampleRackDesigns()[example];op.newInstance=juce::Uuid();op.destination=destination;
    if(housePlate)op.design->id=juce::Uuid();
    op.destination.role=op.design->role;op.allocation=std::move(allocation);op.allowSlotReuse=reuse;
    SoundDocument d;std::uint64_t revision=0;
    CHECK(p.trySoundSnapshot(d,revision));
    const auto status=p.applyRackOperation(op,revision);
    CHECK(status==RackCandidateStatus::ready);
    if(status!=RackCandidateStatus::ready)std::cout<<"  placement status "<<(int)status<<'\n';
    return op.newInstance;
}
// The house prototype's drawn state (Document::setDrawnState): Ember Reed in synth bank 1, Glass Current in bank 2,
// Thread Press on the master chain with its compressor in effect bank 4; then macros 1 to 4 on their focused controls
// AIR +40, FACET -25, ECHO +60 and GRIP +30. With `houseGlass`, Glass Current is placeRack's house-plate copy.
void drawnRacks(InstrumentProcessor& p,bool houseGlass=false)
{
    placeRack(p,0,{},{{0,0},{1,0}},true);
    placeRack(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}},false,houseGlass);
    placeRack(p,2,{RackRole::fx,masterChain,0},{{0,3},{1,4}});
}
void drawnMacros(InstrumentProcessor& p)
{
    const std::pair<const char*,float> bound[]{{"synth.1.analog.filter.cutoff",0.4f},{"synth.2.wavetable.osc.1.position",-0.25f},
                                               {"effect.2.delay.mix",0.6f},{"effect.4.compressor.threshold_db",0.3f}};
    auto d=document(p);
    for(std::size_t n=0;n<4;++n)d.macros[n]={parameterRegistry()[idx(bound[n].first)].stateKey,bound[n].second};
    CHECK(edit(p,d));
}
// Each of a house view's controls at its drawn value, as the prototype's setDrawnValues sets them, written as host
// automation is.
void drawnValues(InstrumentProcessor& p,const juce::String& view)
{
    for(const auto& e:studio::studioSkin()->house.elements)
        if(e.view==view&&e.paramId.isNotEmpty())
        {
            auto& c=p.control(idx(e.paramId));
            c.setValueNotifyingHost(c.convertTo0to1((float)(e.kind=="rotary"||e.kind=="fader"?e.drawnValue:e.defaultValue)));
        }
}
// The AI Rack Synth unit over the processor, on the desktop at its own size and unscaled: the house prototype's
// self-test (house/skin/app/Main.cpp, SelfTest), its checks kept. A listener on each bound parameter counts what a
// host sees: gesture begins and ends, and value changes.
struct HouseRig
{
    struct Counts
    {
        int begins=0,ends=0,changes=0;
        bool operator==(const Counts&) const=default;
    };
    struct Counter final:juce::AudioProcessorParameter::Listener
    {
        Counts counts;
        void parameterValueChanged(int,float) override { ++counts.changes; }
        void parameterGestureChanged(int,bool starting) override { ++(starting?counts.begins:counts.ends); }
    };
    InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
    bool closing=false;
    juce::StringArray reported;
    std::map<juce::String,std::unique_ptr<Counter>> counters;
    // The unit sits in a parent, as in the editor: a desktop window's own size is rounded to the display's scale.
    juce::Component holder;
    std::unique_ptr<studio::HouseUnit> unit;
    HouseRig()
    {
        CHECK(blankInstance(p));
        for(const auto& e:house().elements)
            if(e.paramId.isNotEmpty()&&counters.count(e.paramId)==0)
            {
                auto c=std::make_unique<Counter>();
                p.control(idx(e.paramId)).addListener(c.get());
                counters[e.paramId]=std::move(c);
            }
        studio::house::Host host;
        host.frame=[]{return std::shared_ptr<void>();};
        host.closing=[this]{return closing;};
        host.report=[this](const juce::String& s){reported.add(s);};
        // The editor's change: the edit on a snapshot with today's sentences, then the editor's refresh.
        host.change=[this](const std::function<void(SoundDocument&)>& change,const juce::String& caption)
        {
            SoundDocument d;std::uint64_t at=0;
            if(p.trySoundSnapshot(d,at)){change(d);reported.add(caption+(p.applySoundEdit(d,SoundEditOrigin::manual,at)?" complete":" failed; sound preserved"));}
            else reported.add("Sound is busy; retry the edit");
            refresh();
        };
        unit=std::make_unique<studio::HouseUnit>(p,model(),std::move(host));
        holder.setSize(unit->getWidth(),unit->getHeight());holder.addAndMakeVisible(*unit);
        holder.addToDesktop(0);holder.setVisible(true);refresh();pump();
    }
    ~HouseRig() { unit.reset();for(auto& [id,c]:counters)p.control(idx(id)).removeListener(c.get());pump(); }
    const studio::Model& model() const { return *studio::studioSkin(); }
    const studio::house::Model& house() const { return model().house; }
    studio::house::View& view() { return unit->unit; }
    studio::house::View& strip() { return unit->strip; }
    Counts gestures(const juce::String& id) const { return counters.at(id)->counts; }
    std::map<juce::String,Counts> allCounts() const
    { std::map<juce::String,Counts> out;for(const auto& [id,c]:counters)out[id]=c->counts;return out; }
    double physical(const juce::String& id) { auto& c=p.control(idx(id));return c.convertFrom0to1(c.getValue()); }
    std::map<juce::String,double> values() { std::map<juce::String,double> out;for(const auto& [id,c]:counters)out[id]=physical(id);return out; }
    // What a plug-in wrapper does for host automation: the value and its listeners, no gesture.
    void automate(const juce::String& id,double v) { auto& c=p.control(idx(id));c.setValueNotifyingHost(c.convertTo0to1((float)v)); }
    std::uint64_t revision() { SoundDocument d;std::uint64_t r=0;CHECK(p.trySoundSnapshot(d,r));return r; }
    std::map<juce::String,std::pair<int,int>> allGestures() const
    { std::map<juce::String,std::pair<int,int>> out;for(const auto& [id,c]:counters)out[id]={c->counts.begins,c->counts.ends};return out; }
    MacroBinding binding(int macro) { return document(p).macros[(std::size_t)macro-1]; }
    void refresh() { if(unit)unit->refresh(document(p)); }
    juce::Uuid place(std::size_t example,RackDestination destination,std::vector<UnitBinding> allocation,bool reuse=false)
    { const auto id=placeRack(p,example,destination,std::move(allocation),reuse);refresh();return id; }
    void placeDrawnRacks() { drawnRacks(p);refresh(); }
};
bool pick(studio::house::AssignEditor& editor,const juce::String& item)
{
    for(int i=0;i<editor.target.getNumItems();++i)
        if(editor.target.getItemText(i)==item){editor.target.setSelectedId(editor.target.getItemId(i),juce::sendNotificationSync);return true;}
    std::cout<<"  no target "<<item<<'\n';
    return false;
}
void press(studio::house::SkinButton& b) { clickAt(b,b.element.rect.getCentre()); }
void pressRail(studio::house::FoldRail& rail)
{
    const Pt at((float)rail.getWidth()/2.0f,34.0f);
    rail.mouseDown(event(rail,at,juce::ModifierKeys::leftButtonModifier,1,false));
    rail.mouseUp(event(rail,at,{},1,false));
}
double meanLuma(const juce::Image& img) { return meanLuma(img,img.getBounds().toFloat()); }
double lumaStd(const juce::Image& img)
{
    const auto mean=meanLuma(img);
    const juce::Image::BitmapData bd(img,juce::Image::BitmapData::readOnly);
    double sum=0;
    for(int y=0;y<bd.height;++y) for(int x=0;x<bd.width;++x) { const auto d=luma(bd.getPixelColour(x,y))-mean;sum+=d*d; }
    return std::sqrt(sum/std::max(1,bd.width*bd.height));
}
const juce::String rejectedSentence="Edit rejected; showing current sound (a recall or another gesture may have won)";
void sessionBranding()
{
    std::cout<<"SECTION session_branding\n";
    const auto bytes=studio::house::houseSkinFile("ai-rack-badge.png");
    const auto badge=bytes.getSize()>0?juce::ImageFileFormat::loadFrom(bytes.getData(),bytes.getSize()):juce::Image{};
    CHECK(badge.isValid());
    if(!badge.isValid())return;
    SessionRig r;
    auto* view=r.unit->getChildComponent(0);
    CHECK(view!=nullptr);
    if(view==nullptr)return;
    const auto bare=view->createComponentSnapshot(view->getLocalBounds());
    const auto branded=r.unit->createComponentSnapshot(r.unit->getLocalBounds());
    const juce::Rectangle<int> title(98,12,194,86);
    int inside=0,outside=0;
    for(int y=0;y<bare.getHeight();++y)for(int x=0;x<bare.getWidth();++x)
    {
        const auto a=bare.getPixelAt(x,y),b=branded.getPixelAt(x,y);
        const auto delta=std::max({std::abs(a.getRed()-b.getRed()),std::abs(a.getGreen()-b.getGreen()),std::abs(a.getBlue()-b.getBlue()),std::abs(a.getAlpha()-b.getAlpha())});
        if(delta>1)(title.contains(x,y)?inside:outside)++;
    }
    CHECK(inside>1000&&outside==0);
    std::cout<<"  session title pixels="<<inside<<" outside="<<outside<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("brand-session.png"),branded));
}

void starterSound()
{
    std::cout<<"SECTION starter_sound\n";
    // The empty new instance (decision 4 of the studio feedback batch) stays empty and silent; one click puts Copper Arc in.
    // A genuinely new instance, as a host creates it, not the rig's blank recall (which keeps a silent default synth slot).
    InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump(100);
    struct Close{std::unique_ptr<juce::AudioProcessorEditor>& e;~Close(){e.reset();pump();}} close{editor};
    const auto playNote=[&]{
        p.prepareToPlay(48000,512);
        juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),0);
        float peak=0;
        for(int block=0;block<40;++block){audio.clear();p.processBlock(audio,midi);midi.clear();peak=std::max(peak,audio.getMagnitude(0,512));}
        p.releaseResources();return peak;
    };
    CHECK(document(p).layout.synthCount==0&&playNote()<1.0e-6f);
    auto* starter=widget<juce::Button>(*editor,"starter.sound");
    CHECK(starter!=nullptr&&starter->isShowing());
    if(starter==nullptr)return;
    CHECK(starter->getButtonText().containsIgnoreCase("sound")&&starter->getTitle()=="Start with a sound");
    // It sits inside the empty rack area, clear of the + ADD RACK lettering.
    auto* add=widget<juce::Button>(*editor,"rack.add");
    CHECK(add!=nullptr&&add->getScreenBounds().contains(starter->getScreenBounds()));
    if(output!=juce::File())CHECK(writePng(output.getChildFile("starter-empty.png"),snap(*editor)));
    click(*starter,100);
    const auto after=document(p);
    CHECK(after.layout.synthCount==1);
    CHECK(!starter->isShowing());
    const auto peak=playNote();
    std::cout<<"STARTER_PEAK "<<peak<<'\n';
    CHECK(peak>0.001f&&peak<1.0f);
    if(output!=juce::File())CHECK(writePng(output.getChildFile("starter-placed.png"),snap(*editor)));
    // The library path is still how one chooses a rack; with it open on another new instance the starter is out of the way.
    editor.reset();pump();
    InstrumentProcessor other{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
    editor.reset(other.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump(100);
    struct CloseOther{std::unique_ptr<juce::AudioProcessorEditor>& e;~CloseOther(){e.reset();pump();}} closeOther{editor};
    auto* second=widget<juce::Button>(*editor,"starter.sound");auto* secondAdd=widget<juce::Button>(*editor,"rack.add");
    CHECK(second!=nullptr&&second->isShowing()&&secondAdd!=nullptr);
    if(second==nullptr||secondAdd==nullptr)return;
    click(*secondAdd,100);CHECK(!second->isShowing());
}
void builtInVoicesPlay()
{
    std::cout<<"SECTION built_in_voices_play\n";
    // Every built-in synth rack, alone in a new instance, answers a note: audible, and not clipping.
    std::size_t voices=0;
    for(const auto& design:factoryRackDesigns())
    {
        if(design.role!=RackRole::synth)continue;
        ++voices;
        InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
        SoundDocument d;std::uint64_t revision=0;CHECK(p.trySoundSnapshot(d,revision));
        RackOperation op;op.design=design;op.newInstance=juce::Uuid();
        op.destination={RackRole::synth,masterChain,0};op.allocation={{0,0}};op.allowSlotReuse=true;
        CHECK(p.applyRackOperation(op,revision)==RackCandidateStatus::ready);
        p.prepareToPlay(48000,512);
        juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),0);
        float peak=0;
        for(int block=0;block<60;++block){audio.clear();p.processBlock(audio,midi);midi.clear();peak=std::max(peak,audio.getMagnitude(0,512));}
        p.releaseResources();
        std::cout<<"VOICE_PEAK "<<design.name<<' '<<peak<<'\n';
        CHECK(peak>0.005f&&peak<1.0f);
    }
    CHECK(voices>=2);
}
// Documentation captures use owned component callbacks only: no OS input, clipboard, browser or account service.
void manualScreenshots()
{
    if(output==juce::File{})return;
    InstrumentProcessor p{WindowRig::offline(),airack::testing::isolatedRackRuntime()};
    SoundDocument sound;CHECK(makeFactorySound(0,sound));
    CHECK(restore(p,state(sound)));
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    editor->addToDesktop(0);editor->setVisible(true);pump(150);
    const auto press=[&](const char* id){if(auto* b=widget<juce::Button>(*editor,id)){b->triggerClick();pump(100);}};
    CHECK(writePng(output.getChildFile("first-sound.png"),snap(*editor)));
    press("ai.toggle");
    CHECK(find(*editor,"ai.panel")!=nullptr&&find(*editor,"ai.panel")->isShowing());
    CHECK(writePng(output.getChildFile("ai-account.png"),snap(*editor)));
    press("ai.mcp");
    CHECK(writePng(output.getChildFile("mcp-access.png"),snap(*editor)));
    auto* panel=dynamic_cast<FeedbackPanel*>(find(*editor,"feedback.panel"));CHECK(panel!=nullptr);
    if(panel)
    {
        FeedbackPanel::Host host;
        host.environment=[]{feedback::Environment e;e.operatingSystem="Windows 11";e.host="Example music project";e.format="VST3";e.sampleRate=48000;e.blockSize=256;return e;};
        host.logLines=[]{return juce::StringArray{};};
        panel->setHost(std::move(host));panel->open();pump(60);
        if(auto* message=widget<juce::TextEditor>(*editor,"feedback.message"))message->setText("I expected my saved sound to return. Here is what happened, and how to repeat it.",true);
        pump(60);CHECK(panel->isShowing());
        CHECK(!panel->reportText().contains("@"));
        CHECK(writePng(output.getChildFile("feedback-report.png"),snap(*editor)));
        panel->close();
    }
    editor.reset();pump();
}
void feedbackPanel()
{
    std::cout<<"SECTION feedback_panel\n";
    InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump(100);
    struct Close{std::unique_ptr<juce::AudioProcessorEditor>& e;~Close(){e.reset();pump();}} close{editor};
    auto* badge=find(*editor,"alpha.badge");
    auto* panel=dynamic_cast<FeedbackPanel*>(find(*editor,"feedback.panel"));
    CHECK(badge!=nullptr&&panel!=nullptr&&!panel->isShowing());
    if(badge==nullptr||panel==nullptr)return;
    auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("air-feedback-"+juce::Uuid().toString());
    std::vector<juce::URL> opened;
    FeedbackPanel::Host host;
    host.environment=[]{feedback::Environment e;e.operatingSystem="Windows 11";e.host="Test host";e.format="VST3";e.sampleRate=44100;e.blockSize=256;return e;};
    host.logLines=[]{return juce::StringArray{"12:00 test log line"};};
    host.openUrl=[&](const juce::URL& u){opened.push_back(u);};
    host.saveFolder=[&]{return folder;};
    panel->setHost(host);

    // The badge opens it, over the whole window, with a new report ID.
    clickAt(*badge,badge->getBounds().getCentre().toFloat());pump(60);
    CHECK(panel->isShowing()&&panel->getBounds()==editor->getLocalBounds());
    const auto id=panel->reportId();
    CHECK(feedback::isReportId(id));
    auto* message=widget<juce::TextEditor>(*editor,"feedback.message");
    auto* email=widget<juce::TextEditor>(*editor,"feedback.email");
    auto* preview=widget<juce::TextEditor>(*editor,"feedback.preview");
    auto* details=widget<juce::ToggleButton>(*editor,"feedback.details");
    CHECK(message!=nullptr&&email!=nullptr&&preview!=nullptr&&details!=nullptr&&details->getToggleState());
    if(message==nullptr||email==nullptr||preview==nullptr||details==nullptr)return;
    CHECK(preview->isReadOnly());
    auto text=[&]{return preview->getText();};
    CHECK(text().contains(id)&&text().contains(airack::releaseVersion)&&text().contains("Windows 11")&&text().contains("12:00 test log line"));

    // What is typed shows in the report at once; the preview is exactly what Copy and Save use.
    // The edit boxes announce a change on the next message, so let it arrive.
    message->setText("My sound stops after I save.",true);pump(60);
    CHECK(text().contains("My sound stops after I save."));
    email->setText("me@example.com",true);pump(60);
    CHECK(text().contains("Email: me@example.com"));
    CHECK(text()==panel->reportText());
    click(*details);
    CHECK(!details->getToggleState()&&!text().contains("Windows 11")&&!text().contains("test log line")&&text().contains("My sound stops"));
    click(*details);
    CHECK(details->getToggleState()&&text().contains("Windows 11"));
    CHECK(text().contains("Last session ended normally: yes"));

    // Nothing leaves the panel, or touches the disk, until a button is pressed.
    CHECK(opened.empty()&&!folder.exists());
    click(*widget<juce::Button>(*editor,"feedback.copy"));
    CHECK(juce::SystemClipboard::getTextFromClipboard()==panel->reportText());
    click(*widget<juce::Button>(*editor,"feedback.save"));
    const auto saved=folder.getChildFile("air-feedback-"+id+".txt");
    // The file uses Windows line endings, which Notepad needs.
    CHECK(saved.existsAsFile()&&saved.loadFileAsString().replace("\r\n","\n").trim()==panel->reportText().trim());
    CHECK(opened.empty());
    click(*widget<juce::Button>(*editor,"feedback.open"));
    CHECK(opened.size()==1);
    if(opened.size()==1)
    {
        const auto address=opened[0].toString(true);
        CHECK(address.contains("report="+id)&&address.contains(airack::releaseVersion)&&!address.contains("sound")&&!address.contains("example.com"));
    }
    if(output!=juce::File())CHECK(writePng(output.getChildFile("feedback-panel.png"),snap(*editor)));

    // Close, and a new report is a new one.
    click(*widget<juce::Button>(*editor,"feedback.close"));
    CHECK(!panel->isShowing());
    clickAt(*badge,badge->getBounds().getCentre().toFloat());pump(60);
    CHECK(panel->isShowing()&&panel->reportId()!=id&&message->getText().isEmpty()&&email->getText().isEmpty());
    CHECK(panel->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey))&&!panel->isShowing());

    // After a session that did not end normally the window offers a report, and says so in it.
    auto* notice=find(*editor,"feedback.notice");
    CHECK(notice!=nullptr&&!notice->isShowing());
    if(notice==nullptr)return;
    dynamic_cast<InstrumentEditor&>(*editor).showUnexpectedEndNotice();pump(60);
    CHECK(notice->isShowing());
    if(output!=juce::File())CHECK(writePng(output.getChildFile("feedback-notice.png"),snap(*editor)));
    click(*widget<juce::Button>(*editor,"feedback.notice.dismiss"));
    CHECK(!notice->isShowing()&&!panel->isShowing());
    dynamic_cast<InstrumentEditor&>(*editor).showUnexpectedEndNotice();pump(60);
    click(*widget<juce::Button>(*editor,"feedback.notice.prepare"));
    CHECK(!notice->isShowing()&&panel->isShowing()&&panel->reportText().contains("Last session ended normally: no"));
    panel->close();pump(30);
    folder.deleteRecursively();
}
#if defined(_WIN32) && defined(_DEBUG)
void unexpectedEndNoticeAppears()
{
    std::cout<<"SECTION unexpected_end_notice\n";
    // A session left a marker and is gone; the next window opened in this process offers a report by itself, once.
    auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("air-notice-"+juce::Uuid().toString());
    const auto sessions=root.getChildFile("Sessions");
    CHECK(sessions.createDirectory()&&sessions.getChildFile("1-1.running").replaceWithText("1:1\n"));
    _putenv_s("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",root.getFullPathName().toRawUTF8());
    feedback::SessionTracker::instanceCreated();
    CHECK(feedback::SessionTracker::previousSessionEndedUnexpectedly());
    {
        InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump(100);
        struct Close{std::unique_ptr<juce::AudioProcessorEditor>& e;~Close(){e.reset();pump();}} close{editor};
        auto* notice=find(*editor,"feedback.notice");
        CHECK(notice!=nullptr&&notice->isShowing());
        click(*widget<juce::Button>(*editor,"feedback.notice.prepare"));
        auto* panel=dynamic_cast<FeedbackPanel*>(find(*editor,"feedback.panel"));
        CHECK(panel!=nullptr&&panel->isShowing()&&panel->reportText().contains("Last session ended normally: no"));
        editor.reset();pump();
        // The offer was made once: another window in this process does not repeat it.
        editor.reset(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump(100);
        auto* again=find(*editor,"feedback.notice");
        CHECK(again!=nullptr&&!again->isShowing());
    }
    feedback::SessionTracker::instanceDestroyed();
    CHECK(sessions.getNumberOfChildFiles(juce::File::findFiles,"*.running")==0);
    _putenv_s("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT","");
    root.deleteRecursively();
}
#endif
void libraryOfflineMessage()
{
    std::cout<<"SECTION library_offline_message\n";
    // With no connection, or while the extra racks cannot be fetched, the LIBRARY says so in plain words.
    const auto runtime=airack::testing::isolatedRackRuntime();
    InstrumentProcessor p{WindowRig::offline(), runtime};
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump(100);
    struct Close{std::unique_ptr<juce::AudioProcessorEditor>& e;~Close(){e.reset();pump();}} close{editor};
    // A new window, a started sound and a second of idling make no request of any kind: the library is asked only when opened.
    pump(1000);
    CHECK(runtime->gets->load()==0);
    auto* add=widget<juce::Button>(*editor,"rack.add");
    CHECK(add!=nullptr);
    if(add==nullptr)return;
    click(*add,100);
    juce::String text;
    CHECK(wait("library status",[&]{auto* l=widget<juce::Label>(*editor,"library.status");text=l?l->getText():juce::String();return text.isNotEmpty();}));
    std::cout<<"LIBRARY_STATUS "<<text<<'\n';
    CHECK(runtime->gets->load()>=1);  // opening the library is what asks
    CHECK(text.contains("Can't reach the rack library"));
    CHECK(!text.containsIgnoreCase("Network unavailable")&&!text.containsIgnoreCase("request unavailable")&&!text.contains("OFFLINE"));
    CHECK(text.containsIgnoreCase("built into air"));
    if(output!=juce::File())CHECK(writePng(output.getChildFile("library-offline.png"),snap(*editor)));
}
void alphaBadge()
{
    std::cout<<"SECTION alpha_badge\n";
    // The label is the CMake release version, an alpha prerelease of the numeric product version.
    const juce::String version(airack::releaseVersion);
    CHECK(version.contains("-alpha."));
    {
        // Even an editor with no skin says so.
        InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
        InstrumentEditor bare(p,nullptr);
        bare.setSize(InstrumentEditor::minimumWidth,InstrumentEditor::minimumHeight);
        CHECK(find(bare,"alpha.badge")!=nullptr);
    }
    WindowRig w;
    auto* badge=find(*w.editor,"alpha.badge");
    CHECK(badge!=nullptr&&badge->isShowing());
    if(badge==nullptr)return;
    CHECK(badge->getTitle().containsIgnoreCase("alpha")&&badge->getTitle().contains(version));
    CHECK(!badge->getWantsKeyboardFocus());  // the studio's tab order is unchanged
    auto* tip=dynamic_cast<juce::TooltipClient*>(badge);
    CHECK(tip!=nullptr&&tip->getTooltip().contains(version));
    // In the window's top right corner, wholly inside it, above every unit.
    const auto bounds=badge->getBounds();
    CHECK(w.editor->getLocalBounds().contains(bounds)&&bounds.getRight()>w.editor->getWidth()-16&&bounds.getY()<16);
    {
        // Above every unit: whatever visible sits above it (the tooltip window, the resize corner) does not overlap it.
        const auto index=w.editor->getIndexOfChildComponent(badge);bool clear=index>=0;
        for(int i=index+1;i<w.editor->getNumChildComponents();++i){auto* above=w.editor->getChildComponent(i);
            clear=clear&&(!above->isVisible()||dynamic_cast<juce::TooltipWindow*>(above)!=nullptr||!above->getBounds().intersects(badge->getBounds()));}
        CHECK(clear);
    }
    // The window keeps its size, and the units keep theirs, with the badge there.
    CHECK(w.editor->getWidth()==InstrumentEditor::minimumWidth&&w.editor->getHeight()==InstrumentEditor::minimumHeight);
    w.editor->setSize(1500,900);pump();
    CHECK(w.editor->getLocalBounds().contains(badge->getBounds())&&badge->getBounds().getRight()>1500-16);
    w.editor->setSize(InstrumentEditor::minimumWidth,InstrumentEditor::minimumHeight);pump();
    if(output!=juce::File())CHECK(writePng(output.getChildFile("alpha-badge.png"),snap(*w.editor)));
}
void brandingWindowTitles()
{
    std::cout<<"SECTION branding_window_titles\n";
    struct Window final:juce::DocumentWindow
    {
        Window():DocumentWindow("Host title",juce::Colours::black,0){}
        void closeButtonPressed() override {}
    };
    for(const auto type:{juce::AudioProcessor::wrapperType_VST3,juce::AudioProcessor::wrapperType_Standalone})
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(type);
        InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        CHECK(p.wrapperType==type);
        CHECK(p.getName()=="air");
        InstrumentEditor editor(p,nullptr);
        CHECK(editor.getName()=="air instrument editor");
        Window window;
        window.setContentNonOwned(&editor,false);
        CHECK(window.getName()==(type==juce::AudioProcessor::wrapperType_Standalone?juce::String("air ")+airack::releaseVersion:juce::String("Host title")));
        window.clearContentComponent();
    }
}

void houseBranding()
{
    std::cout<<"SECTION house_branding\n";
    const auto bytes=studio::house::houseSkinFile("ai-rack-wordmark.svg");
    auto logo=bytes.getSize()>0?juce::Drawable::createFromImageData(bytes.getData(),bytes.getSize()):nullptr;
    CHECK(logo!=nullptr);
    HouseRig r;
    CHECK(r.unit->getTitle()=="air unit");
    if(logo==nullptr)return;
    juce::Image mark(juce::Image::ARGB,460,173,true);
    {juce::Graphics g(mark);logo->drawAt(g,0,0,1.0f);}
    // The mirrored R has its upright at the right and its leg pointing left.
    CHECK(mark.getPixelAt(300,90).getAlpha()==0&&mark.getPixelAt(445,90).getAlpha()>240);
    for(const bool folded:{false,true})
    {
        r.unit->setFolded(folded);
        auto& view=folded?r.strip():r.view();
        const auto bare=view.createComponentSnapshot(view.getLocalBounds());
        const auto branded=r.unit->createComponentSnapshot(r.unit->getLocalBounds());
        const juce::Rectangle<int> title=folded?juce::Rectangle<int>(114,48,228,104):juce::Rectangle<int>(116,24,310,120);
        int inside=0,outside=0;
        for(int y=0;y<bare.getHeight();++y)for(int x=0;x<bare.getWidth();++x)
        {
            const auto a=bare.getPixelAt(x,y),b=branded.getPixelAt(x+studio::HouseUnit::railW,y);
            // Parent/child capture rounds a few GDI control-edge pixels by one channel level.
            const auto delta=std::max({std::abs(a.getRed()-b.getRed()),std::abs(a.getGreen()-b.getGreen()),std::abs(a.getBlue()-b.getBlue()),std::abs(a.getAlpha()-b.getAlpha())});
            if(delta>1)(title.contains(x,y)?inside:outside)++;
        }
        CHECK(inside>500&&outside==0);
        std::cout<<"  "<<(folded?"folded":"expanded")<<" title pixels="<<inside<<" outside="<<outside<<'\n';
        if(output!=juce::File{})CHECK(writePng(output.getChildFile(folded?"brand-folded.png":"brand-expanded.png"),branded));
    }
    if(output!=juce::File{})
    {
        CHECK(writePng(output.getChildFile("ai-rack-wordmark.png"),mark));
        CHECK(logo->replaceColour(juce::Colour(0xff282a29),juce::Colour(0xffeeeae0)));
        juce::Image inverse(juce::Image::ARGB,460,173,true);
        {juce::Graphics g(inverse);logo->drawAt(g,0,0,1.0f);}
        CHECK(writePng(output.getChildFile("ai-rack-wordmark-light.png"),inverse));
        for(const auto* name:{"ai-rack-emblem","ai-rack-icon"})
        {
            auto drawable=juce::Drawable::createFromImageFile(juce::File(RACKSYNTH_WORKSPACE).getChildFile("resources/branding/"+juce::String(name)+".svg"));
            CHECK(drawable!=nullptr);
            if(drawable==nullptr)continue;
            const bool icon=juce::String(name).endsWith("icon");
            juce::Image image(juce::Image::ARGB,icon?256:357,icon?256:220,true);
            {juce::Graphics g(image);drawable->drawAt(g,0,0,1.0f);}
            CHECK(writePng(output.getChildFile(juce::String(name)+".png"),image));
        }
    }
}

void houseSkinLoads()
{
    std::cout<<"SECTION house_skin_loads\n";
    const auto& m=studio::studioSkin()->house;
    int elements=0,controls=0,missing=0;
    for(const auto& e:m.elements)
    {
        for(const auto& [role,key]:e.layers)
            if(!key.startsWith("procedural:")&&key!="unit:tape.text"&&m.assets.count(key)==0)++missing;
        // The narrow strip's copies of the strip's elements are counted by house_narrow_skin.
        if(e.view=="narrow")continue;
        ++elements;
        if(e.kind!="tape"&&e.kind!="disclosure")++controls;
    }
    HouseRig r;
    CHECK(elements==69&&controls==48&&missing==0);
    CHECK(r.view().sliders.size()+r.view().buttons.size()+r.strip().sliders.size()+r.strip().buttons.size()==(std::size_t)controls);
    // Both views bind the same 30 parameters, each the registry's with its range, default and choices.
    std::set<juce::String> bound;
    int unlike=0;
    for(const auto& e:m.elements)
    {
        if(e.paramId.isEmpty())continue;
        bound.insert(e.paramId);
        const auto& d=parameterRegistry()[idx(e.paramId)];
        const bool same=e.bindingType=="float"?d.kind==ParameterKind::continuous&&std::abs(e.range.getStart()-d.minimum)<1e-6&&std::abs(e.range.getEnd()-d.maximum)<1e-6
            :e.bindingType=="choice"?d.kind==ParameterKind::choice&&e.choices==juce::StringArray(d.choices.data(),(int)d.choices.size())
            :d.kind==ParameterKind::toggle;
        if(!same||std::abs(e.defaultValue-d.defaultValue)>1e-6){++unlike;std::cout<<"  unlike the registry: "<<e.id<<'\n';}
    }
    CHECK(bound.size()==30&&unlike==0);
    CHECK(m.lettering.face!=nullptr&&m.lettering.face->getName()=="Permanent Marker");
    const auto source=juce::JSON::parse(juce::File(RACKSYNTH_WORKSPACE).getChildFile("resources/house-skin/SOURCE.json"));
    int files=0,shared=0;
    if(auto* list=source["files"].getArray())
        for(const auto& f:*list)
        {
            ++files;
            const auto bytes=studio::house::houseSkinFile(f["path"].toString());
            CHECK(bytes.getSize()>0&&juce::SHA256(bytes).toHexString()==f["sha256"].toString());
        }
    if(auto* list=source["shared"].getArray())
        for(const auto& f:*list)
        {
            ++shared;
            const auto bytes=studio::studioSkinFile(f["embedded_as"].toString().fromFirstOccurrenceOf("resources/studio-skin/",false,false));
            CHECK(bytes.getSize()>0&&juce::SHA256(bytes).toHexString()==f["sha256"].toString());
        }
    CHECK(files==31&&shared==2);
    CHECK(studio::house::houseSkinFile("plates/missing.png").getSize()==0);
}
// The narrow-width rule: the house strip without its wordmark and some bare panel, built at load from the cuts the
// skin records, each joined by a cross-fade, with the strip's elements moved left by the cuts before them.
void houseNarrowSkin()
{
    std::cout<<"SECTION house_narrow_skin\n";
    const auto& m=studio::studioSkin()->house;
    const auto record=juce::JSON::parse(studio::house::houseSkinFile("elements.json").toString())["narrow"];
    if(!record.isObject()||m.assets.count("narrow:ear")==0||m.assets.count("narrow:plate")==0||m.assets.count("narrow:fill")==0)
    {
        CHECK(!"no narrow strip");
        return;
    }
    std::vector<std::pair<int,int>> cuts;
    for(const auto& c:*record["cuts"].getArray())cuts.emplace_back((int)c[0],(int)c[1]);
    const int blend=(int)record["blend"],at=(int)record["fill_at"],width=(int)record["width"],half=blend/2;
    const auto& strip=m.asset("strip:plate").image;
    const auto &ear=m.asset("narrow:ear"),&plate=m.asset("narrow:plate"),&fill=m.asset("narrow:fill");
    int cut=0;
    for(const auto& [a,b]:cuts)cut+=b-a;
    CHECK(cuts.size()==11&&blend==16&&at==118&&width==1549&&width==strip.getWidth()-cut&&width<=1557);
    CHECK(ear.image.getWidth()==at&&ear.image.getHeight()==197&&ear.anchor==Pt{});
    CHECK(plate.image.getWidth()==width-at&&plate.image.getHeight()==197&&plate.anchor==Pt((float)at,0.0f));
    CHECK(fill.image.getWidth()==33&&fill.image.getHeight()==197);
    if(ear.image.getWidth()+plate.image.getWidth()!=width)return;
    const auto narrowAt=[&](int x,int y){return x<at?ear.image.getPixelAt(x,y):plate.image.getPixelAt(x-at,y);};
    // Outside the joins each column is the strip's, the cuts before it skipped; in a join each channel lies between
    // the two columns it fades from and to.
    int unlike=0,unblended=0,x=0,source=0;
    const auto copy=[&](int to)
    {
        for(;source<to;++source,++x)
            for(int y=0;y<197;++y)unlike+=narrowAt(x,y)!=strip.getPixelAt(source,y)?1:0;
    };
    const auto between=[](juce::uint8 v,juce::uint8 p,juce::uint8 q){return (int)v>=std::min((int)p,(int)q)-1&&(int)v<=std::max((int)p,(int)q)+1;};
    for(const auto& [a,b]:cuts)
    {
        copy(a-half);
        for(int i=0;i<blend;++i,++x)
            for(int y=0;y<197;++y)
            {
                const auto v=narrowAt(x,y),p=strip.getPixelAt(a-half+i,y),q=strip.getPixelAt(b-half+i,y);
                unblended+=between(v.getRed(),p.getRed(),q.getRed())&&between(v.getGreen(),p.getGreen(),q.getGreen())&&between(v.getBlue(),p.getBlue(),q.getBlue())?0:1;
            }
        source=b+half;
    }
    copy(strip.getWidth());
    CHECK(x==width&&unlike==0&&unblended==0);
    int unfilled=0;
    for(int fx=0;fx<33;++fx)for(int y=0;y<197;++y)unfilled+=fill.image.getPixelAt(fx,y)!=strip.getPixelAt(359+fx,y)?1:0;
    CHECK(unfilled==0);
    const auto find=[&](const juce::String& id)->const studio::house::Element*{for(const auto& e:m.elements)if(e.id==id)return &e;return nullptr;};
    int copies=0,misplaced=0;
    for(const auto& e:m.elements)
    {
        if(e.view!="strip")continue;
        const auto* n=find("narrow."+e.localId());
        if(n==nullptr){CHECK(!"a strip element has no narrow copy");continue;}
        ++copies;
        float shift=0;
        for(const auto& [a,b]:cuts)if((float)b<=e.footprint().getCentreX())shift+=(float)(b-a);
        const auto sameRect=[](Rect p,Rect q){return p.getTopLeft().getDistanceFrom(q.getTopLeft())<1e-3f&&p.getBottomRight().getDistanceFrom(q.getBottomRight())<1e-3f;};
        CHECK(n->view=="narrow"&&n->group=="narrow.strip"&&n->kind==e.kind&&n->label==e.label&&n->paramId==e.paramId&&n->layers==e.layers);
        misplaced+=sameRect(n->footprint(),e.footprint().translated(-shift,0.0f))&&n->hasDash==e.hasDash
                   &&sameRect(n->dash,e.dash.translated(e.hasDash?-shift:0.0f,0.0f))&&n->footprint().getX()>=(float)at?0:1;
    }
    CHECK(copies==18&&misplaced==0);
    const auto *master=find("narrow.master"),*macro=find("narrow.macro.8"),*tape=find("narrow.macro-tape.8"),*protect=find("narrow.protect");
    CHECK(master&&macro&&tape&&protect);
    if(master&&macro&&tape&&protect)
    {
        const auto sameX=[](float p,float q){return std::abs(p-q)<1e-3f;};
        CHECK(sameX(master->pivot.x,472.26f-282.0f)&&sameX(master->tickCentre.x,472.01f-282.0f)&&sameX(macro->pivot.x,1728.47f-476.0f));
        CHECK(sameX(tape->centre.x,1728.31f-476.0f)&&sameX(protect->rect.getX(),1890.88f-551.0f)&&sameX(protect->led.x,1915.67f-551.0f));
    }
    const studio::house::Group *was=nullptr,*now=nullptr;
    for(const auto& g:m.groups){if(g.id=="strip.strip")was=&g;if(g.id=="narrow.strip")now=&g;}
    CHECK(was!=nullptr&&now!=nullptr);
    if(was&&now)
    {
        juce::StringArray members,order;
        for(const auto& id:was->members)members.add("narrow."+id.fromFirstOccurrenceOf(".",false,false));
        for(const auto& id:was->focusOrder)order.add("narrow."+id.fromFirstOccurrenceOf(".",false,false));
        CHECK(now->view=="narrow"&&now->label==was->label&&now->members==members&&now->focusOrder==order&&order[0]=="narrow.master");
    }
}
// The narrow-width rule's house unit: in a column narrower than the panel it shows its narrow strip, its controls kept
// at the column's right end and a wider column filled with bare panel; open, its 3U unit scrolls sideways under the
// rail; the focus follows between its views.
void houseNarrow()
{
    std::cout<<"SECTION house_narrow\n";
    HouseRig r;
    auto& u=*r.unit;
    auto d=document(r.p);
    d.macros[0]={parameterRegistry()[idx("synth.3.analog.filter.cutoff")].stateKey,0.4f};
    CHECK(edit(r.p,d));
    r.refresh();
    u.setNarrow(1557);pump();
    CHECK(u.narrowWidth()==1557&&u.showsStrip()&&u.rail.folded&&u.narrow.isVisible()&&!u.unit.isVisible()&&!u.strip.isVisible());
    CHECK(u.getWidth()==44+1557&&u.getHeight()==r.strip().getHeight()&&u.narrow.getX()==44+8&&u.across()==0&&u.narrow.getComponentID()=="house.narrow");
    auto* master=u.narrow.slider<studio::house::Knob>("master");
    auto* tape=u.narrow.tape("macro-tape.1");
    CHECK(master!=nullptr&&tape!=nullptr);
    if(master==nullptr||tape==nullptr)return;
    CHECK(master->getComponentID()=="house.narrow.master"&&tape->text().isNotEmpty()&&tape->text()==r.strip().tape("macro-tape.1")->text());
    r.automate("global.master_level",0.5);
    const auto g0=r.gestures("global.master_level");
    drag(*master,master->element.pivot,master->element.pivot-Pt(0.0f,60.0f));
    const auto g1=r.gestures("global.master_level");
    CHECK(std::abs(r.physical("global.master_level")-0.7)<0.003&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
    // A wider column: the controls keep their places from the right end, and bare panel fills from the ear, its tiles
    // alternately mirrored, the first one mirrored.
    u.setNarrow(1900);pump();
    CHECK(u.getWidth()==44+1900&&u.narrow.getX()==44+351);
    {
        const auto snap=u.createComponentSnapshot(u.getLocalBounds(),true,1.0f);
        const auto& fill=r.house().asset("narrow:fill").image;
        const auto same=[](juce::Colour p,juce::Colour q){return std::abs(p.getRed()-q.getRed())<=1&&std::abs(p.getGreen()-q.getGreen())<=1&&std::abs(p.getBlue()-q.getBlue())<=1;};
        int unlike=0;
        for(int y=0;y<197;++y)
            unlike+=(same(snap.getPixelAt(44+118+10,y),fill.getPixelAt(22,y))?0:1)+(same(snap.getPixelAt(44+118+33+10,y),fill.getPixelAt(10,y))?0:1);
        CHECK(unlike==0);
    }
    // The focus follows the strip's control into the narrow strip and back.
    u.setNarrow(0);u.setFolded(true);pump();
    CHECK(!u.narrow.isVisible()&&u.strip.isVisible()&&u.getWidth()==44+r.strip().getWidth());
    if(auto* m3=r.strip().slider("macro.3"))
    {
        m3->grabKeyboardFocus();
        u.setNarrow(1557);pump();
        CHECK(u.narrow.slider("macro.3")!=nullptr&&u.narrow.slider("macro.3")->hasKeyboardFocus(false));
        u.setNarrow(0);pump();
        CHECK(m3->hasKeyboardFocus(false));
    }
    else CHECK(!"no strip macro 3");
    u.setFolded(false);
    // Open: the 3U unit, scrolled sideways within its range, taking no press under the rail.
    u.setOpen(true);
    CHECK(!u.isOpen());
    u.setNarrow(1557);u.setOpen(true);pump();
    CHECK(u.isOpen()&&!u.showsStrip()&&!u.rail.folded&&u.unit.isVisible()&&!u.narrow.isVisible());
    CHECK(u.getWidth()==44+1557&&u.getHeight()==r.view().getHeight()&&u.across()==2140-1557&&u.shift()==0&&u.unit.getX()==44);
    u.setShift(300);
    CHECK(u.shift()==300&&u.unit.getX()==44-300);
    u.setShift(9999);
    CHECK(u.shift()==583&&u.unit.getX()==44-583);
    u.setShift(300);
    auto* under=u.getComponentAt(30,200);
    CHECK(under!=&u.unit&&!u.unit.isParentOf(under));
    int across=0,opened=0;
    u.onAcross=[&](int px){across+=px;};
    u.onOpened=[&]{++opened;};
    juce::MouseWheelDetails w;
    w.deltaX=-0.25f;w.deltaY=0.0f;w.isReversed=w.isSmooth=w.isInertial=false;
    u.unit.mouseWheelMove(event(u.unit,{1000.0f,20.0f},{},0,false),w);
    CHECK(across==56&&u.shift()==300);
    w.deltaX=0.0f;w.deltaY=-0.25f;
    u.unit.mouseWheelMove(event(u.unit,{1000.0f,20.0f},juce::ModifierKeys::shiftModifier,0,false),w);
    CHECK(across==112);
    u.unit.mouseWheelMove(event(u.unit,{1000.0f,20.0f},{},0,false),w);
    CHECK(across==112);
    if(auto* assign=r.view().button("assign.1"))press(*assign);
    CHECK(opened==1&&u.editor.isVisible());
    // Under the pattern the narrow strip shows, with nothing across.
    u.setStripOnly(true);pump();
    CHECK(u.showsStrip()&&u.narrow.isVisible()&&u.across()==0);
    u.setStripOnly(false);pump();
    CHECK(u.unit.isVisible()&&u.shift()==0);
    // Wide again: the unit as its own fold shows it, with nothing across.
    u.setNarrow(0);pump();
    CHECK(!u.isOpen()&&u.across()==0&&u.unit.isVisible()&&u.unit.getX()==44&&u.getWidth()==44+2140);
}
void houseHitRegions()
{
    std::cout<<"SECTION house_hit_regions\n";
    HouseRig r;
    for(auto* v:{&r.view(),&r.strip()})
    {
        r.unit->setFolded(v==&r.strip());
        int ok=0,total=0;
        juce::StringArray misses;
        auto probe=[&](bool passed,const juce::String& what){++total;ok+=passed?1:0;if(!passed)misses.add(what);};
        probe(v->isShowing(),"view showing");
        for(auto& s:v->sliders)
        {
            const auto& e=s->element;
            const auto inside=e.kind=="rotary"?e.pivot:Pt(e.capCentreX,dynamic_cast<studio::house::Fader&>(*s).capY());
            const auto outside=e.kind=="rotary"?e.pivot+Pt(e.hitR+4.0f,0.0f):Pt(e.hitRect.getX()-5.0f,e.hitRect.getCentreY());
            probe(v->getComponentAt(inside.toInt())==s.get(),e.localId()+" centre");
            probe(v->getComponentAt(outside.toInt())!=s.get(),e.localId()+" outside");
        }
        for(auto& b:v->buttons)
        {
            const auto& e=b->element;
            probe(v->getComponentAt(e.rect.getCentre().toInt())==b.get(),e.localId()+" centre");
            probe(v->getComponentAt(Pt(e.rect.getX()-7.0f,e.rect.getCentreY()).toInt())!=b.get(),e.localId()+" outside");
        }
        for(auto& t:v->tapes)
            probe(dynamic_cast<studio::house::Tape*>(v->getComponentAt(t->element.centre.toInt()))==nullptr,t->element.localId()+" passes clicks");
        CHECK(ok==total&&total==(v==&r.view()?89:29));
        if(!misses.isEmpty())std::cout<<"  "<<v->name<<" missed: "<<misses.joinIntoString(", ")<<'\n';
    }
    r.unit->setFolded(false);
}
void houseReadouts()
{
    std::cout<<"SECTION house_readouts\n";
    HouseRig r;
    for(auto* v:{&r.view(),&r.strip()})
    {
        r.unit->setFolded(v==&r.strip());
        juce::StringArray covers;
        for(auto& s:v->sliders)
        {
            v->showReadout(*s);
            const auto box=v->readoutBounds().toFloat();
            if(box.isEmpty()||box.intersects(s->element.footprint())||!v->getLocalBounds().toFloat().contains(box))covers.add(s->element.localId());
            for(auto& t:v->tapes)if(box.intersects(t->element.footprint()))covers.add(s->element.localId()+" over "+t->element.localId());
            v->hideReadouts();
        }
        CHECK(covers.isEmpty());
        if(!covers.isEmpty())std::cout<<"  "<<v->name<<" readouts cover: "<<covers.joinIntoString(", ")<<'\n';
    }
    r.unit->setFolded(false);
    // The prototype's readout text (ruling 8).
    const auto& m=r.house();
    auto element=[&](const char* id)->const studio::house::Element&
    { for(const auto& e:m.elements)if(e.id==id)return e;CHECK(false);return m.elements.front(); };
    using studio::house::formatValue;
    CHECK(formatValue(element("unit.glide"),0.25)=="250 ms"&&formatValue(element("unit.glide"),1.5)=="1.50 s");
    CHECK(formatValue(element("unit.gain"),-5.0)=="-5.0 dB"&&formatValue(element("unit.gain"),2.0)=="+2.0 dB");
    CHECK(formatValue(element("unit.pan.2"),-0.5)=="L 50"&&formatValue(element("unit.pan.2"),0.0)=="C"&&formatValue(element("unit.pan.2"),0.25)=="R 25");
    CHECK(formatValue(element("unit.macro.1"),0.4)=="40%"&&formatValue(element("unit.master"),0.7)=="0.70");
    CHECK(formatValue(element("unit.protect"),1.0)=="on"&&formatValue(element("unit.legato"),0.0)=="off");
    CHECK(formatValue(element("unit.mono"),1.0)=="Monophonic");
}
void houseKnobDrag()
{
    std::cout<<"SECTION house_knob_drag\n";
    HouseRig r;
    auto* master=r.view().slider<studio::house::Knob>("master");
    auto* twin=r.strip().slider<studio::house::Knob>("master");
    CHECK(master!=nullptr&&twin!=nullptr);
    if(master==nullptr||twin==nullptr)return;
    r.automate("global.master_level",0.5);
    const auto g0=r.gestures("global.master_level");
    drag(*master,master->element.pivot,master->element.pivot-Pt(0.0f,60.0f));
    const auto g1=r.gestures("global.master_level");
    CHECK(std::abs(r.physical("global.master_level")-0.7)<0.003);
    CHECK(g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
    using studio::house::pointerAngleFor;
    CHECK(std::abs(twin->pointerAngle()-pointerAngleFor(twin->element,0.7))<0.2f&&std::abs(master->pointerAngle()-pointerAngleFor(master->element,0.7))<0.2f);
}
void houseFaderDrag()
{
    std::cout<<"SECTION house_fader_drag\n";
    HouseRig r;
    auto* level=r.view().slider<studio::house::Fader>("level.1");
    CHECK(level!=nullptr);
    if(level==nullptr)return;
    r.automate("synth.1.level",0.25);
    const auto y0=level->capY();
    const auto g0=r.gestures("synth.1.level");
    const Pt start(level->element.capCentreX,y0);
    drag(*level,start,start-Pt(0.0f,52.0f));
    const auto g1=r.gestures("synth.1.level");
    CHECK(std::abs(y0-level->capY()-52.0f)<1.0f&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
}
void houseButtons()
{
    std::cout<<"SECTION house_buttons\n";
    HouseRig r;
    auto* protect=r.view().button("protect");
    auto* protectStrip=r.strip().button("protect");
    auto* mono=r.view().button("mono");
    auto* mute=r.view().button("mute.3");
    CHECK(protect!=nullptr&&protectStrip!=nullptr&&mono!=nullptr&&mute!=nullptr);
    if(protect==nullptr||protectStrip==nullptr||mono==nullptr||mute==nullptr)return;
    const auto g0=r.gestures("output.protection");
    press(*protect);
    const auto g1=r.gestures("output.protection");
    CHECK(r.physical("output.protection")<0.5&&!protect->lit()&&!protectStrip->lit()&&g1.begins==g0.begins+1&&g1.ends==g0.ends+1);
    press(*protectStrip);
    CHECK(r.physical("output.protection")>0.5&&protect->lit()&&protectStrip->lit());
    const auto m0=r.gestures("global.voice_mode");
    press(*mono);
    const auto m1=r.gestures("global.voice_mode");
    CHECK(juce::roundToInt(r.physical("global.voice_mode"))==1&&mono->lit()&&m1.begins==m0.begins+1&&m1.ends==m0.ends+1);
    press(*mono);
    CHECK(juce::roundToInt(r.physical("global.voice_mode"))==0&&!mono->lit());
    press(*mute);
    CHECK(r.physical("synth.3.mute")>0.5&&mute->lit());
    press(*mute);
    CHECK(r.physical("synth.3.mute")<0.5&&!mute->lit());
}
void houseButtonStates()
{
    std::cout<<"SECTION house_button_states\n";
    HouseRig r;
    auto& unit=r.view();
    auto* b=unit.button("legato");
    CHECK(b!=nullptr);
    if(b==nullptr)return;
    const auto area=b->element.rect.expanded(6.0f).getSmallestIntegerContainer();
    const auto centre=local(*b,b->element.rect.getCentre()),off=local(*b,b->element.rect.getCentre()+Pt(80.0f,0.0f));
    const auto rest=unit.createComponentSnapshot(area,true,1.0f);
    b->mouseEnter(event(*b,centre,{},0,false));
    const auto hover=unit.createComponentSnapshot(area,true,1.0f);
    b->mouseDown(event(*b,centre,juce::ModifierKeys::leftButtonModifier,1,false));
    const auto down=unit.createComponentSnapshot(area,true,1.0f);
    b->mouseDrag(event(*b,off,juce::ModifierKeys::leftButtonModifier,1,true));
    b->mouseUp(event(*b,off,{},1,true));
    b->mouseExit(event(*b,off,{},0,false));
    CHECK(r.physical("global.legato")<0.5);
    CHECK(meanLuma(hover)>meanLuma(rest)+0.5&&meanLuma(down)<meanLuma(rest)-1.0);
    b->setEnabled(false);
    const auto disabled=unit.createComponentSnapshot(area,true,1.0f);
    press(*b);
    CHECK(r.physical("global.legato")<0.5);
    CHECK(lumaStd(disabled)<0.6*lumaStd(rest)&&meanLuma(disabled)<meanLuma(rest)+3.0);
    std::cout<<"  LEGATO face: rest "<<meanLuma(rest)<<", hover "<<meanLuma(hover)<<", pressed "<<meanLuma(down)
             <<"; disabled contrast "<<lumaStd(rest)<<" -> "<<lumaStd(disabled)<<'\n';
    b->setEnabled(true);
}
void houseKeys()
{
    std::cout<<"SECTION house_keys\n";
    HouseRig r;
    auto& unit=r.view();
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&unit))order.add(c->getProperties()["element"].toString());
    CHECK(order==unit.focusOrder&&order.size()==38);
    if(order!=unit.focusOrder)std::cout<<"  Tab order: "<<order.joinIntoString(" ")<<'\n';
    auto* macro3=unit.slider<studio::house::Knob>("macro.3");
    auto* protect=unit.button("protect");
    CHECK(macro3!=nullptr&&protect!=nullptr);
    if(macro3==nullptr||protect==nullptr)return;
    r.automate("macro.3",0.5);
    macro3->grabKeyboardFocus();
    const auto k0=r.gestures("macro.3");
    const auto p0=macro3->proportion();
    macro3->keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    const auto p1=macro3->proportion();
    macro3->keyPressed(juce::KeyPress(juce::KeyPress::upKey,juce::ModifierKeys::shiftModifier,0));
    const auto p2=macro3->proportion();
    const auto k1=r.gestures("macro.3");
    CHECK(macro3->hasKeyboardFocus(false)&&std::abs(p1-p0-0.01)<0.002&&std::abs(p2-p1-0.001)<0.0005&&k1.begins==k0.begins+2&&k1.ends==k0.ends+2);
    macro3->moveKeyboardFocusToSibling(true);
    auto* next=juce::Component::getCurrentlyFocusedComponent();
    CHECK(next!=nullptr&&next->getProperties()["element"].toString()=="unit.assign.3");
    if(auto* assign3=unit.button("assign.3"))CHECK(assign3->showsFocusRing());
    const auto s0=r.gestures("output.protection");
    protect->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    const auto s1=r.gestures("output.protection");
    CHECK(r.physical("output.protection")<0.5&&s1.begins==s0.begins+1&&s1.ends==s0.ends+1);
    protect->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(r.physical("output.protection")>0.5);
}
void houseAutomation()
{
    std::cout<<"SECTION house_automation\n";
    HouseRig r;
    auto* a=r.view().slider<studio::house::Knob>("macro.1");
    auto* b=r.strip().slider<studio::house::Knob>("macro.1");
    CHECK(a!=nullptr&&b!=nullptr);
    if(a==nullptr||b==nullptr)return;
    using studio::house::pointerAngleFor;
    const auto g0=r.gestures("macro.1");
    r.automate("macro.1",0.9);
    const auto g1=r.gestures("macro.1");
    CHECK(std::abs(a->pointerAngle()-pointerAngleFor(a->element,0.9))<0.2f&&std::abs(b->pointerAngle()-pointerAngleFor(b->element,0.9))<0.2f
          &&g1.begins==g0.begins&&g1.ends==g0.ends);
    r.automate("output.protection",0.0);
    const auto pg=r.gestures("output.protection");
    CHECK(!r.view().button("protect")->lit()&&!r.strip().button("protect")->lit()&&pg.begins==pg.ends);
    r.automate("output.protection",1.0);
    // While the user holds MACRO 1 its gesture stays open, so a host's touch automation yields until the release.
    const auto from=local(*a,a->element.pivot);
    const auto h0=r.gestures("macro.1");
    a->mouseDown(event(*a,from,juce::ModifierKeys::leftButtonModifier,1,false));
    a->mouseDrag(event(*a,from-Pt(0.0f,6.0f),juce::ModifierKeys::leftButtonModifier,1,true,from));
    const auto held=r.gestures("macro.1");
    CHECK(held.begins==h0.begins+1&&held.ends==h0.ends);
    a->mouseUp(event(*a,from-Pt(0.0f,6.0f),{},1,true,from));
    CHECK(r.gestures("macro.1").ends==h0.ends+1);
}
void houseFold()
{
    std::cout<<"SECTION house_fold\n";
    HouseRig r;
    auto& unit=r.view();
    auto& strip=r.strip();
    auto* macro3=unit.slider<studio::house::Knob>("macro.3");
    auto* pan2=unit.slider<studio::house::Knob>("pan.2");
    auto* gain=unit.slider<studio::house::Knob>("gain");
    CHECK(macro3!=nullptr&&pan2!=nullptr&&gain!=nullptr);
    if(macro3==nullptr||pan2==nullptr||gain==nullptr)return;
    macro3->grabKeyboardFocus();
    const auto before=r.values();
    const auto counts=r.allCounts();
    const auto revision=r.revision();
    // The rail letters no 3U or 1U: its old lettering box is the column's colour.
    const auto quiet=[&]{const auto box=meanChannels(snap(r.unit->rail),Rect(0.0f,56.0f,42.0f,24.0f));return std::abs(box[0]-0x14)<=1.0&&std::abs(box[1]-0x15)<=1.0&&std::abs(box[2]-0x17)<=1.0;};
    CHECK(r.unit->rail.getComponentID()=="house.fold"&&r.unit->rail.getTitle()=="Fold the air unit");
    const studio::house::FoldRail rack("rack.fold","Fold Ember Reed");
    CHECK(rack.getComponentID()=="rack.fold"&&rack.getTitle()=="Fold Ember Reed");
    pressRail(r.unit->rail);
    CHECK(r.unit->isFolded()&&strip.isVisible()&&!unit.isVisible()&&r.unit->getHeight()==strip.getHeight()&&r.unit->getHeight()==197);
    CHECK(quiet());
    CHECK(r.values()==before&&r.allCounts()==counts&&r.revision()==revision);
    CHECK(juce::Component::getCurrentlyFocusedComponent()==strip.control("macro.3"));
    r.automate("synth.2.pan",-0.5);
    CHECK(std::abs(pan2->pointerAngle()-studio::house::pointerAngleFor(pan2->element,0.25))<0.2f);
    r.unit->rail.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    CHECK(!r.unit->isFolded()&&unit.isVisible()&&juce::Component::getCurrentlyFocusedComponent()==unit.control("macro.3"));
    CHECK(quiet());
    r.automate("synth.2.pan",0.0);
    gain->grabKeyboardFocus();
    pressRail(r.unit->rail);
    CHECK(juce::Component::getCurrentlyFocusedComponent()==&r.unit->rail);
    pressRail(r.unit->rail);
    CHECK(!r.unit->isFolded()&&r.unit->getHeight()==591);
}
// While another gesture holds the processor, each kind of edit changes nothing, shows the parameter again and says
// today's sentence; once it ends, edits work.
void houseRefused()
{
    std::cout<<"SECTION house_refused\n";
    HouseRig r;
    auto* macro1=r.view().slider<studio::house::Knob>("macro.1");
    auto* protect=r.view().button("protect");
    CHECK(macro1!=nullptr&&protect!=nullptr);
    if(macro1==nullptr||protect==nullptr)return;
    const auto other=idx("synth.1.enabled");
    r.automate("macro.1",0.5);
    CHECK(r.p.beginParameterGesture(other));
    const auto before=r.values();
    drag(*macro1,macro1->element.pivot,macro1->element.pivot-Pt(0.0f,60.0f));
    CHECK(r.values()==before&&std::abs(macro1->proportion()-0.5)<1e-6&&r.reported.contains(rejectedSentence));
    r.reported.clear();
    macro1->keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(r.values()==before&&std::abs(macro1->proportion()-0.5)<1e-6&&r.reported.contains(rejectedSentence));
    r.reported.clear();
    press(*protect);
    CHECK(r.values()==before&&protect->lit()&&r.reported.contains(rejectedSentence));
    CHECK(r.p.endParameterGesture(other));
    r.reported.clear();
    macro1->keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(std::abs(r.physical("macro.1")-0.51)<0.002&&r.reported.isEmpty());
}
// While the editor retires nothing writes and nothing is reported; a held drag still ends its gesture once.
void houseClosing()
{
    std::cout<<"SECTION house_closing\n";
    {
        HouseRig r;
        auto* macro1=r.view().slider<studio::house::Knob>("macro.1");
        auto* protect=r.view().button("protect");
        CHECK(macro1!=nullptr&&protect!=nullptr);
        if(macro1==nullptr||protect==nullptr)return;
        r.closing=true;
        const auto before=r.values();
        const auto counts=r.allCounts();
        macro1->keyPressed(juce::KeyPress(juce::KeyPress::upKey));
        press(*protect);
        CHECK(r.values()==before&&r.allCounts()==counts&&r.reported.isEmpty());
    }
    for(const bool destroyed:{false,true})
    {
        HouseRig r;
        auto* macro1=r.view().slider<studio::house::Knob>("macro.1");
        CHECK(macro1!=nullptr);
        if(macro1==nullptr)return;
        const auto from=local(*macro1,macro1->element.pivot);
        const auto g0=r.gestures("macro.1");
        macro1->mouseDown(event(*macro1,from,juce::ModifierKeys::leftButtonModifier,1,false));
        macro1->mouseDrag(event(*macro1,from-Pt(0.0f,6.0f),juce::ModifierKeys::leftButtonModifier,1,true,from));
        const auto held=r.physical("macro.1");
        if(destroyed)r.unit.reset();
        else
        {
            r.closing=true;
            macro1->mouseDrag(event(*macro1,from-Pt(0.0f,30.0f),juce::ModifierKeys::leftButtonModifier,1,true,from));
            macro1->mouseUp(event(*macro1,from-Pt(0.0f,30.0f),{},1,true,from));
        }
        const auto g1=r.gestures("macro.1");
        CHECK(g1.begins==g0.begins+1&&g1.ends==g0.ends+1&&std::abs(r.physical("macro.1")-held)<1e-6&&r.reported.isEmpty());
    }
}
// At the drawn values, each view's render is its plate wherever no control or tape is.
void housePlateIntact()
{
    std::cout<<"SECTION house_plate_intact\n";
    HouseRig r;
    for(auto* v:{&r.view(),&r.strip()})
    {
        r.unit->setFolded(v==&r.strip());
        drawnValues(r.p,v->name);
        juce::Component::unfocusAllComponents();
        v->hideReadouts();
        const auto drawn=v->createComponentSnapshot(v->getLocalBounds(),true,1.0f);
        const auto& plate=r.house().asset(v->name+":plate").image;
        CHECK(drawn.getWidth()==plate.getWidth()&&drawn.getHeight()==plate.getHeight());
        juce::RectangleList<int> covered;
        for(auto& s:v->sliders)covered.add(s->getBounds());
        for(auto& b:v->buttons)covered.add(b->getBounds());
        for(auto& t:v->tapes)covered.add(t->element.footprint().getSmallestIntegerContainer());
        const juce::Image::BitmapData a(drawn,juce::Image::BitmapData::readOnly),b(plate,juce::Image::BitmapData::readOnly);
        int worst=0;
        long long compared=0;
        for(int y=0;y<std::min(a.height,b.height);++y)
            for(int x=0;x<std::min(a.width,b.width);++x)
            {
                if(covered.containsPoint(juce::Point<int>(x,y)))continue;
                const auto p=a.getPixelColour(x,y),q=b.getPixelColour(x,y);
                worst=std::max({worst,std::abs((int)p.getRed()-(int)q.getRed()),std::abs((int)p.getGreen()-(int)q.getGreen()),std::abs((int)p.getBlue()-(int)q.getBlue())});
                ++compared;
            }
        CHECK(worst<=1&&compared>(long long)b.width*b.height/4);
        std::cout<<"  "<<v->name<<": "<<compared<<" pixels outside the controls, worst "<<worst<<'\n';
    }
    r.unit->setFolded(false);
}
// Decision 9's empty rack space alone, at the house unit's scale wide and beside a side unit, as the strip at the rack
// area's foot and filling it: its rails under the house unit's ears on the rack's pitch, + ADD RACK over the interior
// between them, the hint beside or under the lettering, the slot's outline while pointed at or held, and the accent
// ring only when Tab gave it the focus. With --images: space-states.png, the strip in those states.
void emptySpace()
{
    std::cout<<"SECTION empty_space\n";
    const auto& m=*studio::studioSkin();
    // The house skin's rack pitch: the 19 in panel 2140 px wide and the 1.75 in unit 197.11 px tall.
    CHECK(std::abs(m.house.panelWidth-2140.0f)<0.01f&&std::abs(m.house.unitHeight-197.11f)<0.01f);
    const float mm=197.11f/44.45f,rail=(float)studio::HouseUnit::railW,unitWidth=rail+2140.0f;
    juce::Component parent;
    juce::TextButton before;
    studio::EmptyRackSpace space(m);
    before.setBounds(0,0,10,10);
    parent.addAndMakeVisible(before);parent.addAndMakeVisible(space);
    parent.setSize(1547,400);parent.addToDesktop(0);parent.setVisible(true);
    CHECK(space.getComponentID()=="rack.space"&&space.add.getComponentID()=="rack.add"&&space.add.getButtonText()=="+ ADD RACK");
    CHECK(space.add.getTitle()=="Add rack"&&space.hint.getComponentID()=="rack.hint");
    int clicks=0;
    space.add.onClick=[&]{++clicks;};
    for(const int width:{1547,1143})
        for(const int height:{52,335})
        {
            const auto k=(float)width/unitWidth;
            // The strip lies 702 px under the rack area's top, at its foot; the filling space under the house unit.
            const auto depth=height==52?702.0f:591.0f*k;
            space.setRack(k,depth);space.setHint(hintLine);space.setBounds(0,20,width,height);pump();
            // The interior between the rails' faces, which run 0.8125 to 16.6875 mm in from each end of the panel.
            const auto inner=Rect::leftTopRightBottom((rail+16.6875f*mm)*k,0.0f,(rail+2140.0f-16.6875f*mm)*k,(float)height);
            CHECK(closeTo(space.add.getBounds().toFloat(),inner));
            // Each 9.5 mm hole 8.75 mm in from its end of the panel, three a unit at 6.35, 22.225 and 38.1 mm from the
            // rack area's top; every hole that reaches into the space is there, and no other.
            int expected=0,matched=0;
            const auto half=9.5f*mm*k/2.0f;
            for(int u=0;u<8;++u)
                for(const float offset:{6.35f,22.225f,38.1f})
                    for(const float x:{8.75f*mm,2140.0f-8.75f*mm})
                    {
                        const auto centre=Pt((rail+x)*k,((float)u*44.45f+offset)*mm*k-depth);
                        if(centre.y+half<=0.0f||centre.y-half>=(float)height)continue;
                        ++expected;
                        for(const auto& h:space.holes())
                            if(h.getCentre().getDistanceFrom(centre)<=0.5f&&std::abs(h.getWidth()-2.0f*half)<=0.5f&&std::abs(h.getHeight()-2.0f*half)<=0.5f){++matched;break;}
                    }
            CHECK(expected>0&&matched==expected&&(int)space.holes().size()==expected);
            if(matched!=expected||(int)space.holes().size()!=expected)
                std::cout<<"  at "<<width<<" x "<<height<<": "<<space.holes().size()<<" holes, "<<expected<<" expected, "<<matched<<" matched\n";
            // The rails and the column take no clicks; the hint passes them to + ADD RACK.
            CHECK(space.getComponentAt(inner.getCentre().roundToInt())==&space.add);
            CHECK(space.getComponentAt(space.hint.getBounds().getCentre())==&space.add);
            CHECK(space.getComponentAt(juce::Point<int>(2,height/2))==&space);
            if(!space.holes().empty())CHECK(space.getComponentAt(space.holes().front().getCentre().roundToInt())==&space);
            // The hint beside the lettering on the strip, under it when the space fills; the pair centred in the interior.
            const auto letters=space.lettering(),hint=space.hint.getBounds().toFloat(),pair=letters.getUnion(hint);
            CHECK(!letters.isEmpty()&&inner.contains(pair)&&std::abs(pair.getCentreX()-inner.getCentreX())<=1.0f&&std::abs(pair.getCentreY()-inner.getCentreY())<=1.0f);
            if(height==52)CHECK(!space.stacked()&&hint.getX()>=letters.getRight()&&std::abs(letters.getCentreY()-hint.getCentreY())<=1.0f);
            else CHECK(space.stacked()&&hint.getY()>=letters.getBottom()&&std::abs(letters.getCentreX()-hint.getCentreX())<=1.0f);
            // Both sentences fit whole.
            for(const auto& text:{hintLine,libraryHint})
            {
                space.setHint(text);
                CHECK(space.hint.getText()==text&&inner.contains(space.hint.getBounds().toFloat())
                      &&juce::GlyphArrangement::getStringWidth(space.hint.getFont(),text)<=(float)space.hint.getWidth()+0.5f);
            }
            space.setHint(hintLine);
            // The column under the fold rail is the rail's colour.
            const auto column=meanChannels(snap(space),Rect(0.0f,0.0f,std::floor(42.0f*k),(float)height));
            CHECK(std::abs(column[0]-0x14)<=1.0&&std::abs(column[1]-0x15)<=1.0&&std::abs(column[2]-0x17)<=1.0);
            // A press inside, away from the lettering, is one + ADD RACK.
            const int had=clicks;
            clickAt(space.add,{inner.getX()+12.0f,inner.getBottom()-6.0f});
            CHECK(clicks==had+1);
        }
    // The states on the strip, as the window shows it while a rack is placed.
    space.setRack(1547.0f/unitWidth,702.0f);space.setHint(hintLine);space.setBounds(0,20,1547,52);pump();
    const auto accentPixels=[](const juce::Image& img)
    {
        const juce::Image::BitmapData d(img,juce::Image::BitmapData::readOnly);
        int n=0;
        for(int y=0;y<img.getHeight();++y)
            for(int x=0;x<img.getWidth();++x)
            {
                const auto c=d.getPixelColour(x,y);
                if(std::abs(c.getRed()-studio::accent.getRed())<=10&&std::abs(c.getGreen()-studio::accent.getGreen())<=10&&std::abs(c.getBlue()-studio::accent.getBlue())<=10)++n;
            }
        return n;
    };
    // Along the outline's top edge: its brightest pixel, and how many dashes run brighter than 90.
    struct Edge { double peak=0;int dashes=0; };
    const auto edge=[&](const juce::Image& img)
    {
        Edge out;
        const juce::Image::BitmapData d(img,juce::Image::BitmapData::readOnly);
        const auto y=juce::roundToInt(space.slot().getY());
        bool in=false;
        for(int x=juce::roundToInt(space.slot().getX());x<juce::roundToInt(space.slot().getRight());++x)
        {
            const auto l=luma(d.getPixelColour(x,y));
            out.peak=std::max(out.peak,l);
            if(l>90.0&&!in)++out.dashes;
            in=l>90.0;
        }
        return out;
    };
    const auto letters=space.lettering();
    std::vector<juce::Image> states;
    auto shot=[&]{states.push_back(snap(space));return states.back();};
    space.add.setState(juce::Button::buttonNormal);
    const auto idle=shot();
    CHECK(!space.slot().isEmpty()&&edge(idle).peak<70.0&&peakLuma(idle,letters)>=170.0&&peakLuma(idle,letters)<230.0);
    space.add.setState(juce::Button::buttonOver);
    const auto over=shot();
    CHECK(edge(over).peak>=90.0&&edge(over).dashes>=10&&peakLuma(over,letters)>=230.0);
    space.add.setState(juce::Button::buttonDown);
    CHECK(peakLuma(shot(),letters)<160.0);
    space.add.setState(juce::Button::buttonNormal);
    space.add.setToggleState(true,juce::dontSendNotification);
    const auto held=shot();
    CHECK(edge(held).peak>=180.0&&edge(held).dashes>=10&&peakLuma(held,letters)>=230.0);
    space.add.setToggleState(false,juce::dontSendNotification);
    // Focus given by a press shows no ring; focus Tab gave it does.
    space.add.grabKeyboardFocus();pump();
    CHECK(space.add.hasKeyboardFocus(false)&&!space.add.ringed()&&accentPixels(snap(space))==0);
    before.grabKeyboardFocus();before.moveKeyboardFocusToSibling(true);pump();
    CHECK(space.add.hasKeyboardFocus(false)&&space.add.ringed()&&accentPixels(shot())>40);
    if(output!=juce::File{})
    {
        juce::Image sheet(juce::Image::RGB,1547,(int)states.size()*60-8,true);
        {
            juce::Graphics g(sheet);
            for(std::size_t i=0;i<states.size();++i)g.drawImageAt(states[i],0,(int)i*60);
        }
        CHECK(writePng(output.getChildFile("space-states.png"),sheet));
    }
    parent.removeFromDesktop();
}
// With --images <dir>: house/unit-drawn.png and house/strip-drawn.png for house_skin.py compare, each view over the
// drawn state at 1x, unfocused and without a readout, as the prototype's snapshotDrawn renders them.
void houseRenders()
{
    if(output==juce::File{})return;
    std::cout<<"SECTION house_renders\n";
    const auto dir=output.getChildFile("house");
    CHECK(dir.createDirectory());
    HouseRig r;
    r.placeDrawnRacks();
    drawnMacros(r.p);
    r.refresh();
    for(auto* v:{&r.view(),&r.strip()})
    {
        r.unit->setFolded(v==&r.strip());
        drawnValues(r.p,v->name);
        juce::Component::unfocusAllComponents();
        v->hideReadouts();
        CHECK(writePng(dir.getChildFile(v->name+"-drawn.png"),v->createComponentSnapshot(v->getLocalBounds(),true,1.0f)));
    }
    const auto tape=[&](const char* id){ return r.view().tape(id)->text(); };
    CHECK(tape("slot-tape.1")=="EMBER REED"&&tape("slot-tape.2")=="GLASS CURRENT"&&tape("macro-tape.1")=="AIR +40"&&tape("macro-tape.2")=="FACET -25"
          &&tape("macro-tape.3")=="ECHO +60"&&tape("macro-tape.4")=="GRIP +30");
}
// ASSIGN edits the sound document live: the tapes follow in both views, and no parameter moves or gesture goes out.
void houseAssign()
{
    std::cout<<"SECTION house_assign\n";
    HouseRig r;
    r.placeDrawnRacks();
    auto& unit=r.view();
    auto& strip=r.strip();
    auto& ed=r.unit->editor;
    auto* assign5=unit.button("assign.5");
    CHECK(assign5!=nullptr);
    if(assign5==nullptr)return;
    const auto before=r.values();
    const auto gestures=r.allGestures();
    press(*assign5);
    CHECK(ed.isVisible()&&ed.macro==5&&assign5->lit());
    CHECK(pick(ed,"BLOOM   Ember Reed"));
    ed.depth.setValue(25.0,juce::sendNotificationSync);
    CHECK(unit.tape("macro-tape.5")->text()=="BLOOM +25"&&strip.tape("macro-tape.5")->text()=="BLOOM +25");
    ed.clear.onClick();
    CHECK(unit.tape("macro-tape.5")->text().isEmpty()&&r.binding(5)==MacroBinding{});
    CHECK(pick(ed,"AIR   Ember Reed"));
    ed.depth.setValue(-60.0,juce::sendNotificationSync);
    CHECK(unit.tape("macro-tape.5")->text()=="AIR -60");
    ed.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    CHECK(!ed.isVisible()&&!assign5->lit()&&juce::Component::getCurrentlyFocusedComponent()==assign5);
    CHECK(r.values()==before&&r.allGestures()==gestures);
    // Each macro's editor opens beside it, clear of its knob, tape and ASSIGN, and ASSIGN again closes it.
    juce::StringArray covers;
    for(int n=1;n<=8;++n)
    {
        const auto id=juce::String(n);
        auto& b=*unit.button("assign."+id);
        press(b);
        const auto box=ed.getBounds().toFloat();
        if(box.intersects(unit.slider("macro."+id)->element.footprint()))covers.add("macro."+id);
        if(box.intersects(unit.tape("macro-tape."+id)->element.footprint()))covers.add("macro-tape."+id);
        if(box.intersects(b.element.footprint()))covers.add("assign."+id);
        if(!unit.getLocalBounds().toFloat().contains(box))covers.add("editor "+id+" leaves the panel");
        press(b);
    }
    CHECK(covers.isEmpty()&&!ed.isVisible());
    if(!covers.isEmpty())std::cout<<"  editors cover: "<<covers.joinIntoString(", ")<<'\n';
    // The fold and hiding the unit close the editor.
    press(*assign5);
    r.unit->setFolded(true);
    CHECK(!ed.isVisible()&&ed.macro==0&&!assign5->lit());
    r.unit->setFolded(false);
    if(ed.macro!=5)press(*assign5);
    CHECK(ed.macro==5);
    r.unit->setVisible(false);
    CHECK(!ed.isVisible()&&ed.macro==0&&!assign5->lit());
    r.unit->setVisible(true);
}
// A slot tape names the synth rack in its bank; a bank without one shows bare panel.
void houseSlotTapes()
{
    std::cout<<"SECTION house_slot_tapes\n";
    HouseRig r;
    auto& unit=r.view();
    auto* t3=unit.tape("slot-tape.3");
    CHECK(t3!=nullptr);
    if(t3==nullptr)return;
    const auto area=t3->getBounds();
    const auto bare=unit.createComponentSnapshot(area,true,1.0f);
    const auto ember=r.place(0,{},{{0,2},{1,0}});
    const auto named=unit.createComponentSnapshot(area,true,1.0f);
    CHECK(t3->text()=="EMBER REED"&&meanLuma(named)>meanLuma(bare)+20.0);
    RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=ember;
    CHECK(r.p.applyRackOperation(remove,r.revision())==RackCandidateStatus::ready);
    r.refresh();
    const auto again=unit.createComponentSnapshot(area,true,1.0f);
    CHECK(t3->text().isEmpty()&&std::abs(meanLuma(again)-meanLuma(bare))<0.01);
    std::cout<<"  slot 3: bare "<<meanLuma(bare)<<", EMBER REED "<<meanLuma(named)<<", removed "<<meanLuma(again)<<'\n';
}
// An unassigned macro shows the printed dash, as a new project's eight do. The image printed it only for macros 5 to 8,
// in their knobs' columns at their tapes' height; macros 1 to 4 show it there too.
void houseMacroDashes()
{
    std::cout<<"SECTION house_macro_dashes\n";
    HouseRig r;
    for(auto* v:{&r.view(),&r.strip()})
    {
        r.unit->setFolded(v==&r.strip());
        juce::Component::unfocusAllComponents();
        v->hideReadouts();
        const auto drawn=v->createComponentSnapshot(v->getLocalBounds(),true,1.0f);
        for(int n=1;n<=8;++n)
        {
            const juce::String id(n);
            auto* t=v->tape("macro-tape."+id);
            CHECK(t!=nullptr&&t->text().isEmpty()&&t->element.hasDash);
            if(t==nullptr||!t->element.hasDash)continue;
            const auto& dash=t->element.dash;
            const auto c=dash.getCentre().toInt();
            CHECK(drawn.getPixelAt(c.x,c.y).getBrightness()>0.85f);
            const juce::Point<float> place{v->slider("macro."+id)->element.pivot.x,t->element.centre.y};
            if(n>4)
            {
                CHECK(std::abs(dash.getCentreX()-place.x)<2.0f&&std::abs(dash.getCentreY()-place.y)<1.0f);
                continue;
            }
            const auto& printed=v->tape("macro-tape."+juce::String(n+4))->element.dash;
            CHECK(dash.getCentre().getDistanceFrom(place)<0.01f&&dash.getWidth()==printed.getWidth()&&dash.getHeight()==printed.getHeight());
        }
    }
    r.unit->setFolded(false);
}
void houseAssignLocked()
{
    std::cout<<"SECTION house_assign_locked\n";
    HouseRig r;
    auto* assign1=r.view().button("assign.1");
    CHECK(assign1!=nullptr);
    if(assign1==nullptr)return;
    assign1->setEnabled(false);
    press(*assign1);
    CHECK(!r.unit->editor.isVisible());
    assign1->setEnabled(true);
}
// Each pick, non-drag depth change and clear is one sound edit with today's caption; a depth drag is one, on release.
void houseAssignEdits()
{
    std::cout<<"SECTION house_assign_edits\n";
    HouseRig r;
    r.placeDrawnRacks();
    auto& ed=r.unit->editor;
    auto* assign2=r.view().button("assign.2");
    auto* tape2=r.view().tape("macro-tape.2");
    CHECK(assign2!=nullptr&&tape2!=nullptr);
    if(assign2==nullptr||tape2==nullptr)return;
    press(*assign2);
    juce::StringArray items,today;
    for(int i=0;i<ed.target.getNumItems();++i)items.add(ed.target.getItemText(i));
    const auto catalog=parameterRegistry();
    for(std::size_t n=0;n<catalog.size();++n)if(catalog[n].kind==ParameterKind::continuous&&(n==0||n>=9))today.add(catalog[n].name);
    CHECK(items.size()==15+today.size()&&items[0]=="None (unassigned)"&&items[1]=="AIR   Ember Reed");
    juce::StringArray rest;
    for(int i=15;i<items.size();++i)rest.add(items[i]);
    CHECK(rest==today);
    const auto r0=r.revision();
    r.reported.clear();
    CHECK(pick(ed,"ECHO   Glass Current"));
    const auto echo=r.binding(2);
    CHECK(r.revision()==r0+1&&r.reported.contains("Macro assignment complete")&&echo.targetKey==catalog[idx("effect.2.delay.mix")].stateKey);
    CHECK(r.p.undoSoundEdit());
    r.refresh();
    CHECK(r.binding(2)==MacroBinding{}&&ed.target.getSelectedId()==1);
    CHECK(pick(ed,"ECHO   Glass Current"));
    // A drag of ten moves previews on the tape; the document keeps its binding until the release makes one edit.
    auto& depth=ed.depth;
    const auto held=r.binding(2);
    const auto r1=r.revision();
    const Pt from(80.0f,(float)depth.getHeight()/2.0f);
    depth.mouseDown(event(depth,from,juce::ModifierKeys::leftButtonModifier,1,false));
    bool previewed=true;
    for(int i=1;i<=10;++i)
    {
        depth.mouseDrag(event(depth,from+Pt(6.0f*(float)i,0.0f),juce::ModifierKeys::leftButtonModifier,1,true,from));
        const auto pc=juce::roundToInt(depth.getValue());
        previewed=previewed&&tape2->text()=="ECHO "+juce::String(pc<0?"-":"+")+juce::String(std::abs(pc))&&r.binding(2)==held&&r.revision()==r1;
    }
    CHECK(previewed);
    depth.mouseUp(event(depth,from+Pt(60.0f,0.0f),{},1,true,from));
    CHECK(r.revision()==r1+1&&std::abs(r.binding(2).amount-(float)depth.getValue()/100.0f)<1e-6f&&r.binding(2).amount!=held.amount);
    const auto a0=r.binding(2).amount;
    depth.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(r.revision()==r1+2&&std::abs(r.binding(2).amount-(a0+0.001f))<1e-5f);
    r.reported.clear();
    CHECK(pick(ed,"None (unassigned)"));
    CHECK(r.reported.contains("Clear macro complete")&&r.binding(2)==MacroBinding{}&&tape2->text().isEmpty());
}
// A macro tape names its target: the placed rack's label, else the PROGRAMMER's name or the registry's, by bank.
void houseTapeNames()
{
    std::cout<<"SECTION house_tape_names\n";
    HouseRig r;
    const auto& names=r.model().knobNames;
    const auto catalog=parameterRegistry();
    auto bind=[&](int macro,const juce::String& id,float amount)
    {
        auto d=document(r.p);
        d.macros[(std::size_t)macro-1]={catalog[idx(id)].stateKey,amount};
        CHECK(edit(r.p,d));
        r.refresh();
    };
    auto name=[&](const juce::String& id){ const auto i=idx(id);return names[i].isNotEmpty()?names[i]:catalog[i].name.toUpperCase(); };
    auto tape=[&](int n){ return r.view().tape("macro-tape."+juce::String(n))->text(); };
    bind(1,"synth.3.analog.filter.cutoff",0.4f);
    bind(2,"effect.3.delay.mix",-0.25f);
    bind(3,"output.gain_db",0.0f);
    CHECK(names[idx("synth.3.analog.filter.cutoff")].isNotEmpty()&&tape(1)=="S3 "+name("synth.3.analog.filter.cutoff")+" +40");
    CHECK(tape(2)=="FX3 "+name("effect.3.delay.mix")+" -25");
    CHECK(tape(3)==name("output.gain_db")+" +0");
    std::cout<<"  tapes: "<<tape(1)<<" | "<<tape(2)<<" | "<<tape(3)<<'\n';
    r.place(0,{},{{0,2},{1,0}});
    CHECK(tape(1)=="AIR +40"&&r.strip().tape("macro-tape.1")->text()=="AIR +40");
}
// The extracted art draws a slot tape as Tape does, and the stack's tapes from it: tinted, and sized by their text.
void tapeArt()
{
    std::cout<<"SECTION tape_art\n";
    HouseRig r;
    r.placeDrawnRacks();
    auto* t=r.view().tape("slot-tape.1");
    CHECK(t!=nullptr&&t->text()=="EMBER REED");
    if(t==nullptr)return;
    const studio::house::TapeArt art(r.house(),t->element.layer("blank"),t->element.size);
    juce::Image a(juce::Image::ARGB,t->getWidth(),t->getHeight(),true),b(juce::Image::ARGB,t->getWidth(),t->getHeight(),true);
    {juce::Graphics g(a);t->paintEntireComponent(g,true);}
    {juce::Graphics g(b);art.paint(g,t->element.centre-t->getPosition().toFloat(),t->element.angle,t->text(),true);}
    int worst=0;bool inked=false;
    for(int y=0;y<a.getHeight();++y)for(int x=0;x<a.getWidth();++x)
    {
        const auto p=a.getPixelAt(x,y),q=b.getPixelAt(x,y);inked=inked||p.getAlpha()>0;
        worst=std::max({worst,std::abs(p.getRed()-q.getRed()),std::abs(p.getGreen()-q.getGreen()),std::abs(p.getBlue()-q.getBlue()),std::abs(p.getAlpha()-q.getAlpha())});
    }
    CHECK(inked&&worst<=1);
    const auto thread=studio::house::TapeArt::widthFor(r.house(),"THREAD PRESS",54.0f),air=studio::house::TapeArt::widthFor(r.house(),"AIR",54.0f);
    CHECK(air>0.0f&&thread>air);
    // AI: KEEP tinted: its body's mean colour is yellow, read in a band under the body's top edge, above the lettering.
    const studio::house::TapeArt keep(r.house(),t->element.layer("blank"),Pt(150.0f,49.0f));
    juce::Image c(juce::Image::ARGB,200,100,true);
    {juce::Graphics g(c);keep.paint(g,Pt(100.0f,50.0f),0.0f,"AI: KEEP",false,juce::Colour(0xffe9c440));}
    const auto body=meanChannels(c,Rect(40.0f,30.0f,120.0f,5.0f));
    CHECK(body[0]>180.0&&body[1]>180.0&&body[2]<120.0);
    std::cout<<"  widths: THREAD PRESS "<<thread<<", AIR "<<air<<"; AI: KEEP body "<<body[0]<<' '<<body[1]<<' '<<body[2]<<'\n';
}
// An open editor shows the binding the document holds when the sound changes elsewhere.
void houseAssignFollowsSound()
{
    std::cout<<"SECTION house_assign_follows_sound\n";
    HouseRig r;
    r.placeDrawnRacks();
    auto& ed=r.unit->editor;
    auto* assign5=r.view().button("assign.5");
    CHECK(assign5!=nullptr);
    if(assign5==nullptr)return;
    auto d=document(r.p);
    d.macros[4]={parameterRegistry()[idx("effect.4.compressor.threshold_db")].stateKey,0.3f};
    CHECK(edit(r.p,d));
    r.refresh();
    press(*assign5);
    auto shows=[&](const juce::String& target,double depth,const juce::String& tape)
    { return ed.isVisible()&&ed.target.getText()==target&&std::abs(ed.depth.getValue()-depth)<1e-6&&r.view().tape("macro-tape.5")->text()==tape; };
    CHECK(shows("GRIP   Thread Press",30.0,"GRIP +30"));
    CHECK(r.p.undoSoundEdit());
    r.refresh();
    CHECK(shows("None (unassigned)",0.0,""));
    CHECK(r.p.redoSoundEdit());
    r.refresh();
    CHECK(shows("GRIP   Thread Press",30.0,"GRIP +30"));
}
// The unit retiring during a depth drag drops the preview: no edit, nothing reported.
void houseAssignRetires()
{
    std::cout<<"SECTION house_assign_retires\n";
    HouseRig r;
    r.placeDrawnRacks();
    auto* assign2=r.view().button("assign.2");
    CHECK(assign2!=nullptr);
    if(assign2==nullptr)return;
    press(*assign2);
    CHECK(pick(r.unit->editor,"ECHO   Glass Current"));
    auto& depth=r.unit->editor.depth;
    const auto held=r.binding(2);
    const auto at=r.revision();
    r.reported.clear();
    const Pt from(80.0f,(float)depth.getHeight()/2.0f);
    depth.mouseDown(event(depth,from,juce::ModifierKeys::leftButtonModifier,1,false));
    depth.mouseDrag(event(depth,from+Pt(30.0f,0.0f),juce::ModifierKeys::leftButtonModifier,1,true,from));
    CHECK(r.unit->editor.isDragging());
    r.unit.reset();
    CHECK(r.revision()==at&&r.binding(2)==held&&r.reported.isEmpty());
}
// The PROGRAMMER's row rule (Main.cpp's buildRow): SYNTH 1's ANALOG rows put 21 of the 23 drawn knobs within 3 px across
// of where they are drawn (stage C's self-test line 109); the examples' rows and the limit's at the narrowed width by
// the rule's own numbers; a section with no controls takes one place.
void programmerRow()
{
    std::cout<<"SECTION programmer_row\n";
    const auto& P=studio::studioSkin()->programmer;
    std::vector<float> centres;
    for(const auto& counts:{std::vector<int>{5,5,1},std::vector<int>{3,4,5}})
        for(const auto& section:studio::layoutRow(P,counts,93.0f,1452.0f).centres)
            centres.insert(centres.end(),section.begin(),section.end());
    const auto drawn=P["drawn"]["labels"];
    CHECK(centres.size()==23&&drawn.size()==23);
    int near=0;
    float worst=0;
    std::vector<std::pair<juce::String,float>> off;
    for(int i=0;i<std::min(23,(int)centres.size());++i)
    {
        const auto d=std::abs(centres[(std::size_t)i]-(float)(double)drawn[i][2][0]);
        if(d<=3.0f){++near;worst=std::max(worst,d);}
        else off.push_back({drawn[i][0].toString(),d});
    }
    CHECK(near==21&&std::abs(worst-2.72f)<0.01f&&off.size()==2);
    if(off.size()==2)
        CHECK(off[0].first=="LEVEL"&&std::abs(off[0].second-39.37f)<0.01f&&off[1].first=="ATTACK"&&std::abs(off[1].second-4.28f)<0.01f);
    std::cout<<"  "<<near<<" of 23 knobs within 3 px of where drawn, worst "<<worst<<" px\n";
    struct Want { std::vector<int> counts;float right;std::vector<std::vector<float>> centres;std::vector<float> dividers; };
    const std::vector<Want> wants{
        {{2,3,1},1372.0f,{{145.51f,259.77f},{402.59f,516.85f,631.11f},{773.93f}},{331.0f,703.0f}},
        {{2,2},1372.0f,{{145.51f,259.77f},{402.59f,516.85f}},{331.0f}},
        {{3,1},1372.0f,{{145.51f,259.77f,374.03f},{516.85f}},{445.0f}},
        {{2,2,2,2},968.0f,{{145.51f,248.467f},{377.158f,480.115f},{608.806f,711.763f},{840.455f,922.82f}},{313.0f,544.0f,776.0f}},
        {{2,0,1},1372.0f,{{145.51f,259.77f},{},{545.41f}},{331.0f,474.0f}}};
    for(const auto& want:wants)
    {
        const auto row=studio::layoutRow(P,want.counts,93.0f,want.right);
        bool same=row.centres.size()==want.centres.size()&&row.dividers==want.dividers;
        for(std::size_t s=0;same&&s<want.centres.size();++s)
        {
            same=row.centres[s].size()==want.centres[s].size();
            for(std::size_t k=0;same&&k<want.centres[s].size();++k)same=std::abs(row.centres[s][k]-want.centres[s][k])<0.01f;
        }
        CHECK(same);
    }
}
// The PROGRAMMER's section titles (Main.cpp's addTitles) by the rule's own numbers, and SYNTH 1's six ANALOG titles
// from their sections' edges as drawn, worst 2.50 px (stage C's self-test line 109).
void programmerTitles()
{
    std::cout<<"SECTION programmer_titles\n";
    const auto& m=*studio::studioSkin();
    const studio::Lettering caps(m.screenFace),mono(m.face);
    const studio::Faces faces{caps,mono};
    const auto row=studio::layoutTitles(m.programmer,faces,{"OSC 1","OSC 2","NOISE"},{674.0f,1274.0f},93.0f,1453.0f,263.0f,256.0f);
    CHECK(row.titles.size()==3&&row.lines.size()==6);
    const float inkX[]{100.5f,698.5f,1298.5f},ends[]{654.0f,1254.0f,1453.0f};
    for(std::size_t i=0;i<3&&row.titles.size()==3&&row.lines.size()==6;++i)
    {
        const auto ink=row.titles[i].ink;
        const auto& under=row.lines[2*i];
        const auto& rule=row.lines[2*i+1];
        CHECK(row.titles[i].text==juce::StringArray({"OSC 1","OSC 2","NOISE"})[(int)i]&&std::abs(ink.getX()-inkX[i])<0.01f&&!under.upright&&!rule.upright);
        CHECK(under.line==Rect::leftTopRightBottom(std::round(ink.getX())-1.0f,268.0f,std::round(ink.getRight())+1.0f,269.0f));
        CHECK(rule.line==Rect::leftTopRightBottom(std::round(ink.getRight()-1.0f+9.5f),256.0f,ends[i],257.0f));
    }
    const auto drawn=m.programmer["drawn"]["titles"];
    CHECK(drawn.size()==6);
    float worst=0;
    juce::String worstName;
    for(int r=0;r<2&&drawn.size()==6;++r)
    {
        juce::StringArray names;
        std::vector<float> dividers;
        for(int i=3*r;i<3*r+3;++i)
        {
            names.add(drawn[i][0].toString());
            if(i>3*r)dividers.push_back((float)(double)drawn[i][2]-2.0f);
        }
        const auto placed=studio::layoutTitles(m.programmer,faces,names,dividers,93.0f,1453.0f,r==0?263.0f:491.0f,r==0?256.0f:495.0f);
        CHECK(placed.titles.size()==3);
        for(int i=0;i<3&&placed.titles.size()==3;++i)
        {
            const auto d=std::abs(placed.titles[(std::size_t)i].ink.getX()-(float)(double)drawn[3*r+i][1][0]);
            if(d>worst){worst=d;worstName=names[i];}
        }
    }
    CHECK(std::abs(worst-2.5f)<0.01f&&worstName=="NOISE");
    std::cout<<"  six titles from their sections' edges, worst "<<worst<<" px ("<<worstName<<")\n";
}
// The look draws a JUCE button as the PROGRAMMER's sub-page button (Main.cpp's SubpageButton): COMMON over the plate
// where the prototype draws it, against its native snapshot; a toggle as the parts compose it, its LED lit while on.
void programmerButtons()
{
    std::cout<<"SECTION programmer_buttons\n";
    const auto& m=*studio::studioSkin();
    studio::ProgrammerLook look(m);
    const auto& plate=m.plates.at("advanced");
    const auto over=[&](juce::Component& c,juce::Rectangle<int> at)
    {
        auto img=plate.getClippedImage(at).createCopy();
        juce::Graphics g(img);
        c.paintEntireComponent(g,true);
        return img;
    };
    const auto native=juce::File(RACKSYNTH_WORKSPACE).getChildFile("docs/design/racks/2026-10-09-studio-rack/window/skin/compare/native");
    const auto stage=juce::ImageFileFormat::loadFrom(native.getChildFile("stage-advanced.png"));
    CHECK(stage.getWidth()==1547&&stage.getHeight()==965);
    studio::KeyFocus<juce::TextButton> common("Common");
    common.setLookAndFeel(&look);
    common.setSize(84,96);
    if(stage.isValid())matchesPrototype(over(common,{981,29,84,96}),stage.getClippedImage({981,136,84,96}),"COMMON over the plate");
    studio::KeyFocus<juce::ToggleButton> analog("Analog");
    analog.setLookAndFeel(&look);
    analog.setSize(84,96);
    const juce::Rectangle<int> at{1068,29,84,96};
    const Pt led(42.0f,30.0f-13.84f);
    auto want=plate.getClippedImage(at).createCopy();
    {
        juce::Graphics g(want);
        studio::drawSprite(g,m.asset("programmer:button"),{22.0f,30.0f});
        studio::drawSprite(g,m.asset("programmer:led"),led);
        const studio::Lettering caps(m.screenFace),mono(m.face);
        const auto legend=studio::setText({caps,mono},studio::styleOf(m.programmer,"legend"),"ANALOG",0.0f,91.0f,0.0f,1.0f,42.0f);
        g.setColour(juce::Colour::greyLevel(243.0f/255.0f));
        g.fillPath(legend.path);
    }
    matchesPrototype(over(analog,at),want,"ANALOG off, as the parts compose it");
    const auto lens=Rect(8.0f,8.0f).withCentre(led);
    const auto dark=meanLuma(over(analog,at),lens);
    analog.setToggleState(true,juce::dontSendNotification);
    const auto lit=meanLuma(over(analog,at),lens);
    CHECK(lit>dark+40.0);
    const auto& glow=m.asset("programmer:led.lit");
    CHECK(Rect(84.0f,96.0f).contains(Rect((float)glow.image.getWidth(),(float)glow.image.getHeight()).withPosition(led-glow.anchor)));
    std::cout<<"  ANALOG's lens "<<dark<<" unlit, "<<lit<<" lit\n";
    common.setLookAndFeel(nullptr);
    analog.setLookAndFeel(nullptr);
}
// The look draws a rotary slider as the PROGRAMMER's knob (Main.cpp's Knob): the body at its centre, the pointer turned
// by (2p - 1) x 144.5 degrees about its anchor; at 1.15 times both scale; disabled, the pattern unit's veil covers it.
void programmerKnob()
{
    std::cout<<"SECTION programmer_knob\n";
    const auto& m=*studio::studioSkin();
    studio::ProgrammerLook look(m);
    const auto& body=m.asset("programmer:knob.body");
    const auto& pointer=m.asset("programmer:knob.pointer");
    const auto& plate=m.plates.at("advanced");
    studio::KeyFocus<juce::Slider> knob(juce::Slider::RotaryHorizontalVerticalDrag,juce::Slider::NoTextBox);
    knob.setLookAndFeel(&look);
    knob.setRange(0.0,1.0);
    const auto shown=[&]
    {
        auto img=plate.getClippedImage({120,300,knob.getWidth(),knob.getHeight()}).createCopy();
        juce::Graphics g(img);
        knob.paintEntireComponent(g,true);
        return img;
    };
    for(const int size:{103,118})
        for(const double v:{0.0,0.5,1.0})
        {
            knob.setSize(size,size);
            knob.setValue(v,juce::dontSendNotification);
            auto want=plate.getClippedImage({120,300,size,size}).createCopy();
            {
                juce::Graphics g(want);
                g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                const auto k=(float)size/103.0f;
                const Pt c((float)size*0.5f,(float)size*0.5f);
                studio::drawSprite(g,body,c,k);
                g.drawImageTransformed(pointer.image,juce::AffineTransform::translation(-pointer.anchor.x,-pointer.anchor.y).scaled(k)
                                                         .rotated(juce::degreesToRadians((float)(2.0*v-1.0)*144.5f)).translated(c.x,c.y));
            }
            matchesPrototype(shown(),want,(juce::String(size)+" px knob at "+juce::String(v,1)).toRawUTF8());
        }
    knob.setSize(103,103);
    knob.setValue(0.5,juce::dontSendNotification);
    auto veiled=shown();
    {
        juce::Graphics g(veiled);
        g.setColour(juce::Colour(0xff2b2c2e).withAlpha(0.55f));
        g.fillEllipse(Rect(72.5f,72.5f).withCentre({51.5f,51.5f}).expanded(1.0f));
    }
    knob.setEnabled(false);
    matchesPrototype(shown(),veiled,"disabled knob under the veil");
    knob.setLookAndFeel(nullptr);
}
// As the studio's other controls, a control the look draws is ringed in the accent only when Tab gave it the focus;
// an entry shows its outline while it has the focus.
void programmerFocus()
{
    std::cout<<"SECTION programmer_focus\n";
    const auto& m=*studio::studioSkin();
    studio::ProgrammerLook look(m);
    juce::Component parent;
    studio::KeyFocus<juce::Slider> first(juce::Slider::RotaryHorizontalVerticalDrag,juce::Slider::NoTextBox);
    studio::KeyFocus<juce::Slider> second(juce::Slider::RotaryHorizontalVerticalDrag,juce::Slider::NoTextBox);
    juce::TextEditor entry;
    parent.setLookAndFeel(&look);
    for(auto* c:std::initializer_list<juce::Component*>{&first,&second,&entry}){c->setWantsKeyboardFocus(true);parent.addAndMakeVisible(*c);}
    first.setBounds(10,10,103,103);
    second.setBounds(130,10,103,103);
    entry.setBounds(10,130,220,40);
    parent.setSize(250,180);
    parent.addToDesktop(0);
    parent.setVisible(true);
    pump();
    const auto accentPixels=[](const juce::Image& img)
    {
        const juce::Image::BitmapData d(img,juce::Image::BitmapData::readOnly);
        int n=0;
        for(int y=0;y<img.getHeight();++y)
            for(int x=0;x<img.getWidth();++x)
            {
                const auto c=d.getPixelColour(x,y);
                if(std::abs(c.getRed()-studio::accent.getRed())<=10&&std::abs(c.getGreen()-studio::accent.getGreen())<=10&&std::abs(c.getBlue()-studio::accent.getBlue())<=10)++n;
            }
        return n;
    };
    first.grabKeyboardFocus();
    pump();
    CHECK(first.hasKeyboardFocus(false)&&!first.ringed()&&accentPixels(snap(first))==0);
    first.moveKeyboardFocusToSibling(true);
    pump();
    const auto ring=accentPixels(snap(second));
    CHECK(second.hasKeyboardFocus(false)&&second.ringed()&&!first.ringed()&&ring>40);
    entry.grabKeyboardFocus();
    pump();
    const auto outline=accentPixels(snap(entry));
    CHECK(entry.hasKeyboardFocus(true)&&outline>40);
    first.grabKeyboardFocus();
    pump();
    CHECK(accentPixels(snap(entry))==0);
    std::cout<<"  Tab's ring "<<ring<<" accent pixels, the entry's outline "<<outline<<'\n';
    parent.removeFromDesktop();
    parent.setLookAndFeel(nullptr);
}
// The look draws a choice on the PROGRAMMER's display glass: its item in the screen face's capitals 10 px in, centred
// down, and the glass's corners as the plate draws them.
void programmerGlass()
{
    std::cout<<"SECTION programmer_glass\n";
    const auto& m=*studio::studioSkin();
    studio::ProgrammerLook look(m);
    studio::KeyFocus<juce::ComboBox> box;
    box.setLookAndFeel(&look);
    box.addItem("Shared Master",1);
    box.setSelectedId(1,juce::dontSendNotification);
    box.setSize(220,40);
    const auto img=snap(box);
    const auto ink=inkBox(img,{0.0f,0.0f,190.0f,40.0f},128.0);
    const studio::Lettering caps(m.screenFace),mono(m.face);
    const auto want=studio::setText({caps,mono},studio::styleOf(m.screens,"label"),"SHARED MASTER",10.0f,0.0f,0.0f,1.0f).ink();
    CHECK(std::abs(ink.getX()-10.0f)<=1.0f&&std::abs(ink.getCentreY()-20.0f)<=1.0f&&std::abs(ink.getRight()-want.getRight())<=1.0f);
    std::cout<<"  item ink "<<ink.getX()<<" to "<<ink.getRight()<<" across, centred at "<<ink.getCentreY()<<'\n';
    const auto& plate=m.plates.at("advanced");
    const juce::Rectangle<int> glass{93,146,1361,70};
    const std::array<std::array<int,4>,4> corners{{{0,0,glass.getX(),glass.getY()},{210,0,glass.getRight()-10,glass.getY()},
                                                   {0,30,glass.getX(),glass.getBottom()-10},{210,30,glass.getRight()-10,glass.getBottom()-10}}};
    int worst=0;
    for(const auto& c:corners)
        for(int y=0;y<10;++y)
            for(int x=0;x<10;++x)
            {
                const auto p=img.getPixelAt(c[0]+x,c[1]+y),q=plate.getPixelAt(c[2]+x,c[3]+y);
                worst=std::max({worst,std::abs(p.getRed()-q.getRed()),std::abs(p.getGreen()-q.getGreen()),std::abs(p.getBlue()-q.getBlue())});
            }
    CHECK(worst<=1);
    box.setLookAndFeel(nullptr);
}
void windowDefault()
{
    std::cout<<"SECTION window_default\n";
    WindowRig w;
    auto* stage=w.stage();
    CHECK(w.editor->getWidth()==1080&&w.editor->getHeight()==680&&stage!=nullptr);
    if(stage==nullptr)return;
    const auto t=stage->getTransform();
    CHECK(std::abs(t.mat00-0.698f)<0.001f&&std::abs(t.mat11-0.698f)<0.001f&&t.mat01==0.0f&&t.mat10==0.0f);
    // With no rack placed no rack unit shows, and the editor has no rack panel of its own.
    CHECK(w.showing("session")&&w.showing("strip")&&!w.showing("rack.unit.0")&&w.showing("house.panel"));
    CHECK(find(*w.editor,"rack.panel")==nullptr&&find(*w.editor,"rack.viewport")==nullptr&&find(*w.editor,"rack.select")==nullptr);
    CHECK(!w.showing("pages")&&!w.showing("pattern.panel")&&!w.showing("ai.panel")&&!w.showing("library.panel"));
    CHECK(dynamic_cast<studio::PatternUnit*>(find(*w.editor,"pattern.panel"))!=nullptr);
    auto* lever=widget<studio::Lever>(*w.editor,"rack.toggle");
    CHECK(lever!=nullptr&&lever->positionName()=="RACK");
    // The rack area is the stack: the house unit at its top at the stack's scale, then the empty rack space down to the
    // strip (decision 9).
    auto* house=w.house();
    auto* add=widget<juce::Button>(*w.editor,"rack.add");
    auto* hint=widget<juce::Label>(*w.editor,"rack.hint");
    auto* space=widget<studio::EmptyRackSpace>(*w.editor,"rack.space");
    auto* stack=w.stack();
    auto* strip=find(*w.editor,"strip");
    CHECK(house!=nullptr&&stack!=nullptr&&strip!=nullptr);
    if(house==nullptr||add==nullptr||hint==nullptr||space==nullptr||stack==nullptr||strip==nullptr)return;
    const auto unit=stage->getLocalArea(house,house->getLocalBounds().toFloat());
    CHECK(house->getParentComponent()==&stack->body()&&house->isShowing()&&!house->isFolded()&&!house->showsStrip());
    CHECK(std::abs(unit.getX())<0.01f&&std::abs(unit.getY()-107.0f)<0.01f&&std::abs(house->getTransform().mat00-1547.0f/2228.0f)<0.001f);
    CHECK(add->isShowing()&&add->getButtonText()=="+ ADD RACK"&&add->getTitle()=="Add rack"&&!add->getToggleState());
    CHECK(hint->isShowing()&&hint->getText()==hintLine&&hint->getParentComponent()==space&&space->getParentComponent()==&stack->body());
    // rack.stack spans the rack area, from under the session bar to the strip, the stage's third Tab stop.
    auto& view=stack->viewport();
    CHECK(stack->getParentComponent()==stage&&stack->getExplicitFocusOrder()==3&&view.getComponentID()=="rack.stack"
          &&closeTo(stage->getLocalArea(&view,view.getLocalBounds().toFloat()),Rect::leftTopRightBottom(0.0f,107.0f,1547.0f,861.0f)));
    // With no rack, synth 1's generic rack at 3U, then the space from 5U, 1U past the strip, the hint under the lettering.
    auto* bank=find(*w.editor,"rack.bank.synth.1");
    CHECK(bank!=nullptr);
    if(bank==nullptr)return;
    const auto room=w.box(*space),under=w.box(*strip),unitBox=w.box(*house);
    const auto u=stack->unitHeight()*t.mat00;
    CHECK(std::abs(w.box(*bank).getY()-(unitBox.getY()+3.0f*u))<=1.0f&&std::abs(room.getY()-(unitBox.getY()+5.0f*u))<=1.0f
          &&std::abs(room.getBottom()-(unitBox.getY()+6.0f*u))<=1.0f&&room.getBottom()>under.getY()&&space->stacked());
    auto* operation=widget<juce::ComboBox>(*w.editor,"rack.operation");
    CHECK(operation!=nullptr&&itemIds(*operation)==(juce::Array<int>{4,5,6,7,8})&&!w.showing("rack.review"));
    // A placed rack's unit sits at 3U, Ember Reed's faceplate 3U, and the space from 6U at least to the strip, the hint
    // beside the lettering. The 6U are taller than the view, which scrolls to show the selected rack whole.
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    auto* rack=w.rack(0);
    if(rack==nullptr)return;
    const auto shown=w.box(*rack),end=w.box(*space),top=w.box(*house);
    CHECK(std::abs(shown.getY()-(top.getY()+3.0f*u))<=1.0f&&std::abs(shown.getHeight()-3.0f*u)<=1.0f&&shown.getBottom()<=under.getY()+1.0f);
    CHECK(add->isShowing()&&std::abs(end.getY()-(top.getY()+6.0f*u))<=1.0f&&end.getBottom()>=under.getY()-1.0f&&space->stacked());
}
void windowInterlocks()
{
    std::cout<<"SECTION window_interlocks\n";
    WindowRig w;
    auto& e=*w.editor;
    // At 1600 x 1000, where the stack beside a side unit is scaled down whole rather than narrow (the narrow-width rule).
    e.setSize(1600,1000);pump();
    auto* lever=widget<studio::Lever>(e,"rack.toggle");
    auto* ai=widget<studio::SkinButton>(e,"ai.toggle");
    auto* unitFold=widget<studio::Disclosure>(e,"pattern.fold");
    auto* add=widget<juce::Button>(e,"rack.add");
    auto* strip=find(e,"strip");
    auto* house=w.house();
    // The strip's fold as it shows, wide or narrowed.
    auto fold=[&]{return widget<studio::Disclosure>(e,"strip.fold");};
    if(lever==nullptr||ai==nullptr||fold()==nullptr||unitFold==nullptr||add==nullptr||strip==nullptr||house==nullptr)return;
    // A placed rack, so its unit shows where it may.
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    const auto before=w.revisions();
    // What shows (the rack's unit, the LIBRARY, pages, the AI unit, pattern unit, the house unit's unit or strip, the strip
    // narrowed beside a side unit) and what the controls read (ADVANCED, AI lit, both folds unfolded, + ADD RACK held
    // while the LIBRARY is open).
    enum Shown { hidden, unitShown, stripShown };
    auto is=[&](int line,bool rack,bool library,bool pages,bool aiUnit,bool pattern,Shown unit,bool advanced,bool aiLit,bool unfolded)
    {
        const bool houseIs=library||unit==hidden?!house->isShowing():house->isShowing()&&house->showsStrip()==(unit==stripShown);
        const bool shows=w.showing("rack.unit.0")== (rack&&!library)&&w.inLibrary()==library&&w.showing("pages")==pages&&w.showing("ai.panel")==aiUnit
                         &&w.showing("pattern.panel")==pattern&&houseIs&&w.showing("rack.add")== (unit==unitShown&&!library)
                         &&strip->getWidth()==(aiUnit?1143:1547);
        const bool reads=(lever->position()==1)==advanced&&ai->lit()==aiLit&&fold()->expanded==unfolded&&unitFold->expanded==unfolded
                         &&add->getToggleState()==library;
        CHECK(shows&&reads);
        if(!(shows&&reads))std::cout<<"  interlock step at line "<<line<<'\n';
    };
    auto flip=[&]{click(*lever);};
    auto toggleAi=[&]{click(*ai);};
    auto toggleFold=[&]{fold()->press();pump();};
    auto foldUnit=[&]{unitFold->press();pump();};
    auto addRack=[&]{click(*add);};
    // + ADD RACK from each state, pressed again, and the LIBRARY's CANCEL.
    flip();         is(__LINE__,false,false,true,false,false,hidden,true,false,false);
    addRack();      is(__LINE__,true,true,false,false,false,unitShown,false,false,false);
    toggleAi();     is(__LINE__,true,false,false,true,false,unitShown,false,true,false);
    addRack();      is(__LINE__,true,true,false,false,false,unitShown,false,false,false);
    toggleFold();   is(__LINE__,false,false,false,false,true,stripShown,false,false,true);
    addRack();      is(__LINE__,true,true,false,false,false,unitShown,false,false,false);
    CHECK(!add->isShowing()&&w.inLibrary()); // Cancel is the visible Gallery close control.
    flip();         is(__LINE__,false,false,true,false,false,hidden,true,false,false);
    flip();         is(__LINE__,true,false,false,false,false,unitShown,false,false,false);
    addRack();      is(__LINE__,true,true,false,false,false,unitShown,false,false,false);
    click(e,"library.cancel");
                    is(__LINE__,true,false,false,false,false,unitShown,false,false,false);
    // Today's steps, with the house strip and no rack unit while the pattern unit is unfolded.
    flip();         is(__LINE__,false,false,true,false,false,hidden,true,false,false);
    toggleAi();     is(__LINE__,true,false,false,true,false,unitShown,false,true,false);
    toggleFold();   is(__LINE__,false,false,false,false,true,stripShown,false,false,true);
    toggleAi();     is(__LINE__,true,false,false,true,false,unitShown,false,true,false);
    flip();         is(__LINE__,false,false,true,false,false,hidden,true,false,false);
    toggleFold();   is(__LINE__,false,false,false,false,true,stripShown,false,false,true);
    flip();         is(__LINE__,false,false,true,false,false,hidden,true,false,false);
    flip();         is(__LINE__,true,false,false,false,false,unitShown,false,false,false);
    click(e,"rack.operations");
    click(e,"rack.advanced");
                    is(__LINE__,false,false,true,false,false,hidden,true,false,false);
    flip();
    toggleAi();     is(__LINE__,true,false,false,true,false,unitShown,false,true,false);
    toggleAi();     is(__LINE__,true,false,false,false,false,unitShown,false,false,false);
    toggleFold();   is(__LINE__,false,false,false,false,true,stripShown,false,false,true);
    foldUnit();     is(__LINE__,true,false,false,false,false,unitShown,false,false,false);
    CHECK(w.revisions()==before);
}
void windowPatternUnfolds()
{
    std::cout<<"SECTION window_pattern_unfolds\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* unit=dynamic_cast<studio::PatternUnit*>(find(e,"pattern.panel"));
    auto* fold=widget<studio::Disclosure>(e,"strip.fold");
    auto* unitFold=widget<studio::Disclosure>(e,"pattern.fold");
    auto* ai=widget<studio::SkinButton>(e,"ai.toggle");
    auto* stage=w.stage();
    auto* house=w.house();
    CHECK(unit!=nullptr&&house!=nullptr);
    if(unit==nullptr||fold==nullptr||unitFold==nullptr||ai==nullptr||stage==nullptr||house==nullptr)return;
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    const auto before=w.revisions();
    fold->press();pump();
    CHECK(unit->isShowing()&&!w.showing("strip")&&unit->getParentComponent()==stage);
    CHECK(unit->getY()==(int)w.model().layout["pattern"]["y"]&&unit->getBottom()==stage->getHeight());
    // The house strip takes the rack area above the unit; the empty space and the rack's unit hide.
    const auto shown=stage->getLocalArea(house,house->getLocalBounds().toFloat());
    CHECK(house->isShowing()&&house->showsStrip()&&!house->isFolded()&&std::abs(shown.getY()-107.0f)<0.01f&&shown.getBottom()<=263.01f);
    CHECK(!w.showing("rack.unit.0")&&!w.showing("rack.add")&&!w.showing("rack.hint"));
    unitFold->press();pump();
    CHECK(!unit->isShowing()&&w.showing("strip")&&!house->showsStrip()&&w.showing("rack.unit.0")&&w.showing("rack.add"));
    // The user's fold returns when the pattern unit folds.
    pressRail(house->rail);pump();
    fold->press();pump();
    unitFold->press();pump();
    CHECK(house->isFolded()&&house->showsStrip()&&w.showing("rack.unit.0"));
    // The rail, pressed while the pattern unit is unfolded, folds it and unfolds the house unit.
    fold->press();pump();
    pressRail(house->rail);pump();
    CHECK(!unit->isShowing()&&!fold->expanded&&!house->isFolded()&&!house->showsStrip()&&w.showing("rack.unit.0"));
    click(*ai);
    CHECK(w.showing("ai.panel"));
    auto* narrowFold=widget<studio::Disclosure>(e,"strip.fold");
    CHECK(narrowFold!=fold);
    if(narrowFold!=nullptr)narrowFold->press();
    pump();
    CHECK(unit->isShowing()&&!w.showing("ai.panel")&&!ai->lit());
    click(*ai);
    CHECK(!unit->isShowing()&&w.showing("strip")&&w.showing("ai.panel"));
    CHECK(w.revisions()==before);
}
void windowResizes()
{
    std::cout<<"SECTION window_resizes\n";
    juce::TemporaryFile savedPreset(".airack");
    WindowRig w;
    w.p.setSoundPresetPath(savedPreset.getFile().getFullPathName());
    auto& e=*w.editor;
    auto* stage=w.stage();
    auto* save=widget<studio::SkinButton>(e,"preset.save");
    auto* stack=w.stack();
    auto* status=widget<juce::Label>(e,"status");
    auto* house=w.house();
    auto* hint=widget<juce::Label>(e,"rack.hint");
    if(stage==nullptr||save==nullptr||stack==nullptr||status==nullptr||house==nullptr||hint==nullptr)return;
    const auto& layout=w.model().layout;
    const auto top=(float)(int)layout["advanced"]["y"],bottom=(float)(int)layout["strip"]["y"];
    for(auto size:{juce::Point<int>(1600,1200),juce::Point<int>(2200,1600),juce::Point<int>(1081,900)})
    {
        e.setSize(size.x,size.y);pump();
        const auto width=(float)e.getWidth(),height=(float)e.getHeight();
        const auto shown=e.getLocalArea(stage,stage->getLocalBounds().toFloat());
        CHECK(std::abs(shown.getWidth()/shown.getHeight()-1547.0f/965.0f)<0.002f);
        CHECK(std::abs(shown.getX()-(width-shown.getRight()))<=1.0f&&std::abs(shown.getY()-(height-shown.getBottom()))<=1.0f);
        CHECK(std::abs(shown.getWidth()-width)<=1.0f||std::abs(shown.getHeight()-height)<=1.0f);
        const auto centre=e.getLocalPoint(stage,save->element.rect.getCentre());
        CHECK(e.getComponentAt(centre.roundToInt())==save);
        status->setText({},juce::dontSendNotification);
        const auto at=save->getLocalPoint(&e,centre);
        save->mouseDown(event(*save,at,juce::ModifierKeys::leftButtonModifier,1,false));
        save->mouseUp(event(*save,at,{},1,false));
        CHECK(status->getText()=="Save preset complete"&&savedPreset.getFile().existsAsFile());
        // The rack area is the stack's: the unit at its top with its proportions, 2184 of the stack's 2228 design px wide,
        // synth 1's generic rack 2U under it, then the empty space between the rails, 1U past the strip.
        const auto area=e.getLocalArea(stage,Rect(0.0f,top,1547.0f,bottom-top));
        const auto unit=area.withWidth(area.getWidth()*2184.0f/2228.0f).withHeight(area.getWidth()*591.0f/2228.0f);
        const auto k=area.getWidth()/1547.0f;
        const auto room=Rect::leftTopRightBottom(area.getX(),area.getY()+std::round(5.0f*stack->unitHeight())*k,unit.getRight(),area.getY()+std::round(6.0f*stack->unitHeight())*k);
        const auto gotUnit=w.box(*house),gotRoom=w.box(*hint->getParentComponent()),gotStack=w.box(*stack);
        CHECK(closeTo(gotUnit,unit)&&closeTo(gotRoom,room)&&closeTo(gotStack,area));
        if(!closeTo(gotUnit,unit)||!closeTo(gotRoom,room)||!closeTo(gotStack,area))
            std::cout<<"  at "<<size.x<<" x "<<size.y<<": unit "<<gotUnit.toString()<<", space "<<gotRoom.toString()<<", stack "<<gotStack.toString()<<'\n';
    }
}
void windowRendersAtTwiceScale()
{
    std::cout<<"SECTION window_renders_at_twice_scale\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* stage=w.stage();
    auto* display=widget<studio::Display>(e,"session.display");
    if(stage==nullptr||display==nullptr)return;
    const auto one=e.createComponentSnapshot(e.getLocalBounds(),true,1.0f);
    const auto two=e.createComponentSnapshot(e.getLocalBounds(),true,2.0f);
    CHECK(two.getWidth()==2*one.getWidth()&&two.getHeight()==2*one.getHeight());
    CHECK(std::abs(display->renderScale()-2.0f*stage->getTransform().mat00)<0.02f);
    const auto box=e.getLocalArea(stage,w.model().displays.at("session").line("name").ink.expanded(6.0f,4.0f));
    const auto a=inkBox(one,box,0.5*peakLuma(one,box)),b=inkBox(two,box*2.0f,0.5*peakLuma(two,box*2.0f));
    CHECK(!a.isEmpty()&&!b.isEmpty());
    const auto e1=std::max({std::abs(b.getX()/2-a.getX()),std::abs(b.getY()/2-a.getY()),std::abs(b.getRight()/2-a.getRight()),std::abs(b.getBottom()/2-a.getBottom())});
    CHECK(e1<=1.0f);
    if(e1>1.0f)std::cout<<"  ink 1x "<<a.toString()<<" 2x "<<b.toString()<<'\n';
}
void windowPatternAtTwiceScale()
{
    std::cout<<"SECTION window_pattern_at_twice_scale\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* stage=w.stage();
    auto* fold=widget<studio::Disclosure>(e,"strip.fold");
    auto* display=widget<studio::Display>(e,"pattern.settings");
    if(stage==nullptr||fold==nullptr||display==nullptr)return;
    fold->press();pump();
    CHECK(display->isShowing());
    // The window's 1x draws the glass shrunk from its render at one pixel per design pixel, which moves this line's
    // ink edges by up to 1.5 px with the renderer's sampling, so the 2x ink is compared with that render.
    const auto unit=1.0f/stage->getTransform().mat00;
    const auto one=e.createComponentSnapshot(e.getLocalBounds(),true,unit);
    const auto two=e.createComponentSnapshot(e.getLocalBounds(),true,2.0f);
    CHECK(std::abs(display->renderScale()-2.0f*stage->getTransform().mat00)<0.02f);
    const auto top=(float)(int)w.model().layout["pattern"]["y"];
    const auto box=e.getLocalArea(stage,w.model().displays.at("pattern.settings").line("settings").ink.expanded(6.0f,4.0f).translated(0.0f,top));
    const auto a=inkBox(one,box*unit,0.5*peakLuma(one,box*unit))/unit,b=inkBox(two,box*2.0f,0.5*peakLuma(two,box*2.0f))/2.0f;
    CHECK(!a.isEmpty()&&!b.isEmpty());
    const auto e1=std::max({std::abs(b.getX()-a.getX()),std::abs(b.getY()-a.getY()),std::abs(b.getRight()-a.getRight()),std::abs(b.getBottom()-a.getBottom())});
    CHECK(e1<=1.0f);
    if(e1>1.0f)std::cout<<"  ink at one pixel per design pixel "<<a.toString()<<", 2x "<<b.toString()<<'\n';
}
void windowFocusOrder()
{
    std::cout<<"SECTION window_focus_order\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* aiUnit=find(e,"ai.panel");
    auto* display=find(e,"session.display");
    auto* house=w.house();
    if(aiUnit==nullptr||display==nullptr||house==nullptr)return;
    // Without a rack, Tab ends at + ADD RACK; with one, the rack's unit comes between the house unit and + ADD RACK.
    {
        auto* add=find(e,"rack.add");
        CHECK(add!=nullptr&&juce::KeyboardFocusTraverser().getNextComponent(add)==nullptr);
    }
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    const juce::StringArray stops{"session.display","session.save","session.load","session.browse","session.undo","session.redo",
        "session.rack","session.ai","strip.fold","strip.mode","strip.display","strip.export"};
    // The house unit's, the stack's and the faceplates' stops go by their component IDs; the window skin's by their element IDs.
    auto stop=[](juce::Component* x){ const auto id=x->getComponentID();return id=="ai.mcp"||id.startsWith("house.")||id.startsWith("face.")||(id.startsWith("rack.")&&id!="rack.toggle")?id:x->getProperties()["element"].toString(); };
    auto houseStops=[](studio::house::View& v){ juce::StringArray ids{"house.fold"};for(const auto& id:v.focusOrder)ids.add("house."+id);return ids; };
    auto want=stops;
    want.addArray(houseStops(house->unit));
    // Ember Reed's unit: its triangle, its faceplate's six controls and PROGRAMMER pull, then OPERATIONS.
    want.addArray({"rack.fold","face.air","face.bite","face.bloom","face.attack","face.release","face.grain","face.programmer","rack.operations"});
    want.add("rack.add");
    juce::KeyboardFocusTraverser traverser;
    juce::StringArray got;
    auto* c=static_cast<juce::Component*>(display);
    for(int i=0;i<want.size()&&c!=nullptr;++i){got.add(stop(c));c=traverser.getNextComponent(c);}
    CHECK(house->unit.focusOrder.size()==38&&got==want&&c==nullptr);
    if(got!=want)std::cout<<"  Tab order: "<<got.joinIntoString(", ")<<'\n';
    // With the AI unit open, its stops come after + ADD RACK, and Tab ends there.
    if(auto* ai=widget<juce::Button>(e,"ai.toggle")){click(*ai);}
    auto sideWant=stops;
    sideWant.addArray(houseStops(house->unit));
    sideWant.addArray({"rack.fold","face.air","face.bite","face.bloom","face.attack","face.release","face.grain","face.programmer","rack.operations"});
    sideWant.add("rack.add");
    for(const auto& id:w.model().focusOrder.at("ai"))sideWant.add("ai."+id);
    sideWant.add("ai.mcp");
    auto* mcp=find(e,"ai.mcp");
    CHECK(mcp!=nullptr&&mcp->isShowing()&&mcp->getWantsKeyboardFocus());
    std::cout<<"MCP_FOCUS_ACCESS context=window_focus_order id="<<(mcp?mcp->getComponentID():juce::String("<null>"))
             <<" showing="<<(mcp&&mcp->isShowing())<<" wantsKeyboardFocus="<<(mcp&&mcp->getWantsKeyboardFocus())<<'\n';
    juce::StringArray sideGot;
    c=display;
    for(int i=0;i<sideWant.size()&&c!=nullptr;++i){sideGot.add(stop(c));c=traverser.getNextComponent(c);}
    std::cout<<"FOCUS_RESIDUAL context=window_focus_order prefix_equal="<<(sideGot==sideWant)<<" ai_showing="<<aiUnit->isShowing()
             <<" id="<<(c?c->getComponentID():juce::String("<null>"))<<" name="<<(c?c->getName():juce::String("<null>"))
             <<" showing="<<(c&&c->isShowing())<<" wantsKeyboardFocus="<<(c&&c->getWantsKeyboardFocus())<<'\n';
    CHECK(aiUnit->isShowing()&&sideGot==sideWant&&c==nullptr);
    if(sideGot!=sideWant)std::cout<<"  Tab order beside the AI unit: "<<sideGot.joinIntoString(", ")<<'\n';
    // Unfolded, the unit's stops take the strip's place after the session unit, then the house strip's, and Tab ends
    // there; a selected note enables the note's knobs and DELETE.
    WindowRig u;
    auto* unfoldedHouse=u.house();
    auto* fold=widget<studio::Disclosure>(*u.editor,"strip.fold");
    auto* roll=widget<studio::Roll>(*u.editor,"pattern.roll");
    c=find(*u.editor,"session.display");
    if(unfoldedHouse==nullptr||fold==nullptr||roll==nullptr||c==nullptr)return;
    PatternDocument one;
    one.noteCount=1;
    one.notes[0]={0,1,60,100,1};
    ProjectDocument d;std::uint64_t revision=0,reset=0;
    CHECK(u.p.tryProjectSnapshot(d,revision,reset)&&u.p.applyPatternEdit(one,revision));
    fold->press();
    CHECK(wait("the note shown",[&]{return roll->shown().notes.size()==1;}));
    const auto at=roll->noteRect(0).getCentre()-roll->getPosition().toFloat();
    roll->mouseDown(event(*roll,at,juce::ModifierKeys::leftButtonModifier,1,false));
    roll->mouseUp(event(*roll,at,{},1,false));pump();
    CHECK(roll->shown().selected==0);
    juce::StringArray unfoldedWant;
    for(int i=0;i<8;++i)unfoldedWant.add(stops[i]);
    for(const auto& id:u.model().focusOrder.at("pattern"))unfoldedWant.add("pattern."+id);
    const auto patternStops=unfoldedWant.size();
    unfoldedWant.addArray(houseStops(unfoldedHouse->strip));
    juce::StringArray unfoldedGot;
    for(int i=0;i<unfoldedWant.size()&&c!=nullptr;++i){unfoldedGot.add(stop(c));c=traverser.getNextComponent(c);}
    CHECK(patternStops==32&&unfoldedGot==unfoldedWant&&c==nullptr);
    if(unfoldedGot!=unfoldedWant)std::cout<<"  Unfolded Tab order: "<<unfoldedGot.joinIntoString(", ")<<'\n';
}
// ADVANCED shows the PROGRAMMER in the rack area's place, its 1547 x 754 at 0, 107 on the stage and scaled with it, in
// place of the stack, and closes the side units. Showing it edits nothing.
void windowProgrammer()
{
    std::cout<<"SECTION window_programmer\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* stage=w.stage();
    auto* unit=dynamic_cast<studio::ProgrammerUnit*>(find(e,"pages"));
    auto* lever=widget<studio::Lever>(e,"rack.toggle");
    auto* ai=widget<juce::Button>(e,"ai.toggle");
    auto* house=w.house();
    CHECK(unit!=nullptr);
    if(stage==nullptr||unit==nullptr||lever==nullptr||ai==nullptr||house==nullptr)return;
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    const auto before=w.revisions();
    CHECK(unit->getParentComponent()==stage&&!unit->isVisible());
    click(*ai);
    CHECK(w.showing("ai.panel"));
    click(*lever);
    CHECK(unit->isShowing()&&w.showing("strip")&&!house->isShowing()&&!w.showing("rack.add")&&!w.showing("rack.unit.0")
          &&!w.showing("ai.panel")&&!w.inLibrary());
    CHECK(unit->getBounds()==juce::Rectangle<int>(0,(int)w.model().layout["advanced"]["y"],1547,754));
    for(auto size:{juce::Point<int>(1080,680),juce::Point<int>(1600,1200),juce::Point<int>(2200,1600)})
    {
        e.setSize(size.x,size.y);pump();
        const auto want=e.getLocalArea(stage,Rect(0.0f,107.0f,1547.0f,754.0f)),got=w.box(*unit);
        CHECK(closeTo(got,want));
        if(!closeTo(got,want))std::cout<<"  at "<<size.x<<" x "<<size.y<<": the unit "<<got.toString()<<", want "<<want.toString()<<'\n';
    }
    click(*lever);
    CHECK(!unit->isVisible()&&house->isShowing()&&w.showing("rack.unit.0"));
    CHECK(w.revisions()==before);
}
// Under ADVANCED, Tab goes from the session unit and the strip into the PROGRAMMER: its page buttons, the shown page's
// sub-page buttons, the display, then the page's knobs row by row, and ends there. RACK LAYOUT goes down its rows, then
// the selected bank's controls.
void windowProgrammerTabOrder()
{
    std::cout<<"SECTION window_programmer_tab_order\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* unit=dynamic_cast<studio::ProgrammerUnit*>(find(e,"pages"));
    auto* display=find(e,"session.display");
    auto* lever=widget<studio::Lever>(e,"rack.toggle");
    CHECK(unit!=nullptr);
    if(unit==nullptr||display==nullptr||lever==nullptr)return;
    click(*lever);
    CHECK(unit->isShowing());
    const juce::StringArray stops{"session.display","session.save","session.load","session.browse","session.undo","session.redo",
        "session.rack","session.ai","strip.fold","strip.mode","strip.display","strip.export"};
    juce::KeyboardFocusTraverser traverser;
    const std::pair<const char*,const char*> shown[]{{"global",""},{"synth1","analog"},{"synth1","routes"},{"effects","fx1"},{"layout",""}};
    for(const auto& [page,subpage]:shown)
    {
        unit->showPage(page);
        if(*subpage!=0)unit->showSubpage(subpage);
        pump();
        std::vector<juce::Component*> want;
        for(auto& b:unit->pageButtons)want.push_back(b.get());
        for(auto& b:unit->subpageButtons)if(b->isVisible())want.push_back(b.get());
        want.push_back(unit->display.get());
        if(juce::String(page)=="layout")
        {
            for(auto* row:unit->rackRows())want.push_back(row);
            for(const auto* name:{"add","remove","replace","up","down","lock"})want.push_back(find(*unit,juce::String("synth.1.")+name));
        }
        else for(auto* k:unit->shownKnobs())want.push_back(k);
        juce::StringArray session,got,wanted;
        auto* c=display;
        for(int i=0;i<stops.size()&&c!=nullptr;++i){session.add(c->getProperties()["element"].toString());c=traverser.getNextComponent(c);}
        std::vector<juce::Component*> walked;
        while(c!=nullptr&&walked.size()<=want.size()){walked.push_back(c);c=traverser.getNextComponent(c);}
        for(auto* x:walked)got.add(x->getComponentID());
        for(auto* x:want)wanted.add(x!=nullptr?x->getComponentID():juce::String("<missing>"));
        CHECK(session==stops&&walked==want&&c==nullptr);
        if(session!=stops||walked!=want||c!=nullptr)
            std::cout<<"  "<<page<<" "<<subpage<<" Tab order: "<<session.joinIntoString(", ")<<" | "<<got.joinIntoString(", ")
                     <<"\n  want "<<wanted.joinIntoString(", ")<<'\n';
    }
}
// + ADD RACK opens the LIBRARY in the side unit's place; AI, ADVANCED, PATTERN, its CANCEL and an Apply close it.
void windowLibrary()
{
    std::cout<<"SECTION window_library\n";WindowRig w;auto& e=*w.editor;const auto before=w.revisions();
    auto* library=widget<studio::LibraryUnit>(e,"library.panel");auto* strip=find(e,"strip");auto* stage=w.stage();CHECK(library&&strip&&stage&&w.stack());if(!library||!strip||!stage||!w.stack())return;
    for(const auto size:{juce::Point<int>(1080,680),juce::Point<int>(1600,1000)}){
        e.setSize(size.x,size.y);pump();const auto scroll=w.stack()->viewport().getViewPosition();const auto selected=w.stack()->selected();click(e,"rack.add");CHECK(w.inLibrary()&&!w.stack()->isShowing());
        const auto bounds=w.box(*library),session=w.box(*find(e,"session")),pattern=w.box(*strip);CHECK(std::abs(bounds.getX()-w.box(*stage).getX())<=1&&std::abs(bounds.getWidth()-w.box(*stage).getWidth())<=1&&bounds.getY()>=session.getBottom()-1&&bounds.getBottom()<=pattern.getY()+1);
        CHECK(w.revisions()==before);click(e,"library.cancel");CHECK(!w.inLibrary()&&w.stack()->isShowing()&&w.stack()->selected()==selected&&w.stack()->viewport().getViewPosition()==scroll);
    }
    click(e,"ai.toggle");CHECK(w.showing("ai.panel"));click(e,"rack.add");CHECK(w.inLibrary()&&!w.showing("ai.panel"));click(e,"library.cancel");CHECK(w.showing("ai.panel"));click(e,"ai.toggle");
    click(e,"rack.add");CHECK(chooseLibrary(e,"Copper Arc"));click(e,"library.apply");CHECK(wait("Gallery Add closes and selects rack",[&]{return !w.inLibrary()&&w.stack()->unit(w.stack()->selected())!=nullptr;}));CHECK(document(w.p).personalities->instances.back().design.name=="Copper Arc");
}
// Decision 9 on the stage: with no rack placed the empty space fills the rack area under the house unit, so no dark gap
// remains, its holes under the house unit's ear slots; a press in it, away from the lettering, opens the LIBRARY, which
// narrows the space with the unit. With a rack placed the space starts under the rack's unit, at least 1U, and fills to
// the foot, the house unit folded or not.
void windowEmptySpace()
{
    std::cout<<"SECTION window_empty_space\n";
    WindowRig w;
    auto& e=*w.editor;
    // At 1600 x 1000, where the stack beside a side unit is scaled down whole rather than narrow (the narrow-width rule).
    e.setSize(1600,1000);pump();
    auto* space=widget<studio::EmptyRackSpace>(e,"rack.space");
    auto* house=w.house();
    auto* stack=w.stack();
    auto* strip=find(e,"strip");
    auto* stage=w.stage();
    CHECK(stack!=nullptr);
    if(space==nullptr||house==nullptr||stack==nullptr||strip==nullptr||stage==nullptr)return;
    const auto s=stage->getTransform().mat00;
    CHECK(meanLuma(snap(e),w.box(*space))>15.0);
    // Each hole under one of the house plate's ear slots, 39 px in from each end of unit.png and strip.png.
    auto underEars=[&]
    {
        bool ok=!space->holes().empty();
        const auto left=e.getLocalPoint(house,Pt((float)studio::HouseUnit::railW+39.0f,0.0f)).x;
        const auto right=e.getLocalPoint(house,Pt((float)studio::HouseUnit::railW+2101.0f,0.0f)).x;
        for(const auto& h:space->holes())
        {
            const auto x=e.getLocalPoint(space,h.getCentre()).x;
            ok=ok&&std::min(std::abs(x-left),std::abs(x-right))<=1.5f*s;
        }
        return ok;
    };
    CHECK(underEars()&&space->stacked());
    const auto inner=space->add.getBounds().toFloat();
    clickAt(space->add,{inner.getX()+20.0f,inner.getBottom()-20.0f});pump();
    CHECK(w.inLibrary()&&space->add.getToggleState()&&underEars());
    CHECK(std::abs(w.box(*space).getRight()-w.box(*house).getRight())<=1.0f);
    click(e,"library.cancel");
    CHECK(!w.inLibrary()&&!space->add.getToggleState());
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    auto* rack=w.rack(0);
    if(rack==nullptr)return;
    for(const bool folded:{false,true})
    {
        house->setFolded(folded);pump();
        const auto unit=w.box(*rack),foot=w.box(*space),under=w.box(*strip);
        CHECK(std::abs(foot.getY()-unit.getBottom())<=1.0f&&foot.getBottom()>=under.getY()-1.0f&&foot.getHeight()>=stack->unitHeight()*s-1.0f);
        CHECK(space->stacked()&&underEars());
    }
}
// Spec decision 8, in the stack: a rack's unit shows only while the rack is placed. An undo or a host recall that takes
// the rack away retires its unit, takes the focus out of it and ends a drag's gesture (Review Focus 1).
void windowRackPanelShows()
{
    std::cout<<"SECTION window_rack_panel_shows\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* stack=w.stack();
    auto* stage=w.stage();
    auto* lever=widget<studio::Lever>(e,"rack.toggle");
    auto* add=widget<juce::Button>(e,"rack.add");
    CHECK(stack!=nullptr);
    if(stack==nullptr||stage==nullptr||lever==nullptr||add==nullptr)return;
    const auto unitShowing=[&]{return w.showing("rack.unit.0");};
    juce::MemoryBlock empty;
    w.p.getStateInformation(empty);
    CHECK(!unitShowing());
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",unitShowing));
    // AIR, Ember Reed's first control, on its unit's faceplate, and its parameter.
    auto air=[&]()->juce::Slider*
    {
        auto* unit=w.rack(0);
        return unit!=nullptr?dynamic_cast<juce::Slider*>(find(*unit,"face.air")):nullptr;
    };
    std::optional<std::size_t> index;
    {
        const auto design=airack::test::frozenExampleRackDesigns()[0];
        const auto d=document(w.p);
        const auto& c=design.panel.controls[0];
        const auto u=std::find_if(design.units.begin(),design.units.end(),[&](const auto& v){return v.id==c.unit;});
        if(d.personalities!=nullptr&&!d.personalities->instances.empty()&&u!=design.units.end())
        {
            const auto& bindings=d.personalities->instances.back().bindings;
            const auto b=std::find_if(bindings.begin(),bindings.end(),[&](auto v){return v.unit==c.unit;});
            if(b!=bindings.end())index=rackParameterIndex(u->module,b->slot,c.key);
        }
    }
    CHECK(index.has_value());
    // The focus on AIR: an undo of the placement retires the unit, the focus not left in it; a redo shows a unit again.
    auto* slider=air();
    CHECK(slider!=nullptr);
    if(slider!=nullptr){slider->grabKeyboardFocus();pump();CHECK(slider->hasKeyboardFocus(false));}
    juce::Component::SafePointer<juce::Component> retired(w.rack(0));
    CHECK(w.p.undoSoundEdit());
    CHECK(wait("the unit retired after the undo",[&]{return !unitShowing();}));
    pump();
    auto* focused=juce::Component::getCurrentlyFocusedComponent();
    CHECK(retired==nullptr&&(focused==nullptr||!stack->isParentOf(focused))&&stack->selected().isNull());
    CHECK(w.p.redoSoundEdit());
    CHECK(wait("a unit shown after the redo",unitShowing));
    // A drag open on AIR when the host recalls the state from before the placement: the unit goes, ending the gesture.
    slider=air();
    CHECK(slider!=nullptr);
    if(slider!=nullptr&&index)
    {
        slider->onDragStart();
        const bool open=!w.p.beginParameterGesture(*index);
        CHECK(open);
        if(!open)w.p.endParameterGesture(*index);
        w.p.setStateInformation(empty.getData(),(int)empty.getSize());
        CHECK(wait("the unit retired after the recall",[&]{return !unitShowing();}));
        pump();
        CHECK(w.p.beginParameterGesture(*index));
        CHECK(w.p.endParameterGesture(*index));
    }
    // The Gallery hides the full-width stack; Advanced and Rack return through their existing layout states.
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the unit shown again",unitShowing));
    const auto width=[&]{return stage->getLocalArea(stack,stack->getLocalBounds().toFloat()).getWidth();};
    click(*add);
    CHECK(w.inLibrary()&&!unitShowing()&&!stack->isShowing()&&std::abs(width()-1547.0f)<=0.5f);
    click(*lever);
    CHECK(w.showing("pages")&&!unitShowing());
    click(*lever);
    CHECK(unitShowing()&&std::abs(width()-1547.0f)<=0.5f);
}
// The stack selects the first placed rack whenever it has none and the sound has racks: when one is placed, when the
// selected one goes and when the editor opens on them.
void windowFirstRack()
{
    std::cout<<"SECTION window_first_rack\n";
    WindowRig w;
    auto* stack=w.stack();
    CHECK(stack!=nullptr);
    if(stack==nullptr)return;
    // The placed racks in placement order; the n-th selected, with its unit's controls showing: its faceplate or its row.
    auto placed=[&]{std::vector<juce::Uuid> ids;const auto d=document(w.p);if(d.personalities)for(const auto& i:d.personalities->instances)ids.push_back(i.id);return ids;};
    auto shows=[&](std::size_t n)
    {
        const auto ids=placed();
        auto* u=n<ids.size()?stack->unit(ids[n]):nullptr;
        return u!=nullptr&&stack->selected()==ids[n]&&(u->face()!=nullptr?u->face()->isShowing():u->row.isShowing());
    };
    CHECK(stack->selected().isNull());
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack selected",[&]{return shows(0);}));
    // A second rack placed keeps the selection; when the selected one goes, the first is selected again.
    placeRack(w.p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}});
    CHECK(wait("the second rack's unit",[&]{return w.rack(1)!=nullptr;}));
    CHECK(shows(0));
    stack->select(placed()[1]);
    CHECK(shows(1));
    CHECK(w.p.undoSoundEdit());
    CHECK(wait("the first rack selected again",[&]{return shows(0);}));
    // With the last rack gone, none is selected.
    CHECK(w.p.undoSoundEdit());
    CHECK(wait("no rack",[&]{return !w.showing("rack.unit.0")&&stack->selected().isNull();}));
    // An editor opened on a sound with racks selects the first before it is shown, the stack at its top.
    CHECK(w.p.redoSoundEdit());
    CHECK(wait("the rack back",[&]{return shows(0);}));
    w.editor.reset();pump();
    w.editor.reset(w.p.createEditor());
    stack=w.stack();
    if(stack==nullptr)return;
    CHECK(stack->selected()==placed()[0]);
    w.editor->addToDesktop(0);w.editor->setVisible(true);
    CHECK(wait("its controls in the new editor",[&]{return shows(0);}));
    CHECK(stack->viewport().getViewPositionY()==0);
}
// The rail folds the unit to its 1U strip and the rack's unit comes up under it; unfolded, the unit is 3U again with the
// rack's unit under it. Neither way changes the sound.
void windowHouseFold()
{
    std::cout<<"SECTION window_house_fold\n";
    WindowRig w;
    auto* house=w.house();
    auto* stage=w.stage();
    if(house==nullptr||stage==nullptr)return;
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    auto* rack=w.rack(0);
    if(rack==nullptr)return;
    const auto before=w.revisions();
    const auto k=1547.0f/2228.0f*stage->getTransform().mat00;
    pressRail(house->rail);pump();
    const auto unit=w.box(*house);
    CHECK(house->isFolded()&&std::abs(unit.getHeight()-197.0f*k)<=1.0f&&std::abs(w.box(*rack).getY()-unit.getBottom())<=1.0f);
    std::cout<<"  folded: unit "<<unit.getHeight()<<" px tall\n";
    pressRail(house->rail);pump();
    CHECK(!house->isFolded()&&std::abs(w.box(*house).getHeight()-591.0f*k)<=1.0f&&std::abs(w.box(*rack).getY()-w.box(*house).getBottom())<=1.0f);
    CHECK(w.revisions()==before);
}
// AI narrows the house with working hit targets; Gallery hides it and Cancel restores the prior AI composition.
void windowHouseNarrows()
{
    std::cout<<"SECTION window_house_narrows\n";
    WindowRig w;
    auto& e=*w.editor;
    e.setSize(1600,1000);pump();
    auto* house=w.house();
    auto* ai=widget<studio::SkinButton>(e,"ai.toggle");
    auto* add=widget<juce::Button>(e,"rack.add");
    auto* stage=w.stage();
    if(house==nullptr||ai==nullptr||add==nullptr||stage==nullptr)return;
    auto* master=house->unit.slider("master");
    CHECK(master!=nullptr);
    if(master==nullptr)return;
    const auto area=e.getLocalArea(stage,Rect(0.0f,107.0f,1547.0f,861.0f-107.0f));
    for(const bool library:{false,true})
    {
        if(library)click(*add);
        else click(*ai);
        auto* side=find(e,library?"library.panel":"ai.panel");
        if(library){const auto revisions=w.revisions();CHECK(side&&side->isShowing()&&!house->isShowing()&&!w.stack()->isShowing());click(e,"library.cancel");CHECK(w.showing("ai.panel")&&house->isShowing()&&w.revisions()==revisions);continue;}
        const auto unit=w.box(*house);
        CHECK(side!=nullptr&&side->isShowing()&&std::abs(unit.getWidth()-area.getWidth()*1143.0f/1547.0f*2184.0f/2228.0f)<=1.0f&&std::abs(unit.getHeight()/unit.getWidth()-591.0f/2184.0f)<0.001f);
        const auto centre=e.getLocalPoint(&house->unit,master->element.pivot);
        CHECK(e.getComponentAt(centre.roundToInt())==master);
        std::cout<<"  beside the "<<(library?"LIBRARY":"AI unit")<<": unit "<<unit.getWidth()<<" x "<<unit.getHeight()<<'\n';
    }
}
void placeOxide(InstrumentProcessor&,bool);
// Beside a side unit the stack is scaled to the column, so every rack shows whole and none scrolls sideways, at every
// window size (the user's 2026-10-19 decision, which replaces the narrow-width rule's readable size). With no side unit the stack fills the stage, as before. With --images: window-side-ai.png,
// window-side-library.png and window-side-1300.png.
void windowSideScale()
{
    std::cout<<"SECTION window_side_scale\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* ai=widget<studio::SkinButton>(e,"ai.toggle");
    auto* add=widget<juce::Button>(e,"rack.add");
    auto* stack=w.stack();
    auto* house=w.house();
    if(ai==nullptr||add==nullptr||stack==nullptr||house==nullptr)return;
    drawnRacks(w.p);placeOxide(w.p,false);
    CHECK(wait("the racks' units",[&]{return w.rack(3)!=nullptr&&w.rack(3)->face()!=nullptr;}));
    const auto column=1143.0f/1547.0f;
    const auto scale=[&]{return stack->getTransform().mat00;};
    const auto shot=[&](const char* name)
    {
        stack->viewport().setViewPosition(0,0);pump(60);
        if(output!=juce::File{})CHECK(writePng(output.getChildFile(name),snap(e)));
    };
    // The stack as wide as the stage at scale k: no unit folded by the column or shifted sideways, the house on its fold.
    const auto whole=[&](float k,const juce::String& where)
    {
        bool fits=std::abs(scale()-k)<1e-4f&&stack->getWidth()==1547&&!stack->narrow()&&!stack->houseOpen()&&!house->showsStrip();
        for(int n=0;n<4;++n)
        {
            auto* u=w.rack(n);
            fits=fits&&u!=nullptr&&!u->columnFolded()&&!u->isFolded()&&u->across()==0&&u->shift()==0;
        }
        if(!fits)std::cout<<"  not whole "<<where<<": scale "<<scale()<<", width "<<stack->getWidth()<<'\n';
        CHECK(fits);
    };
    whole(1.0f,"at 1080 x 680");
    click(*ai,60);
    CHECK(w.showing("ai.panel"));
    whole(column,"beside the AI unit at 1080 x 680");
    shot("window-side-ai.png");
    click(*add);
    CHECK(w.inLibrary());
    CHECK(!stack->isShowing()&&!house->isShowing());whole(1.0f,"retained behind the Gallery at 1080 x 680");
    shot("window-side-library.png");
    click(*ai);
    for(const auto size:{juce::Point<int>(1300,810),juce::Point<int>(1600,1000)})
    {
        e.setSize(size.x,size.y);pump(60);
        CHECK(w.showing("ai.panel")&&e.getWidth()==size.x&&e.getHeight()==size.y);
        whole(column,"beside the AI unit at "+juce::String(size.x)+" x "+juce::String(size.y));
        if(size.x==1300)shot("window-side-1300.png");
    }
    click(*ai);
    for(const auto size:{juce::Point<int>(1600,1200),juce::Point<int>(1080,680)})
    {
        e.setSize(size.x,size.y);pump();
        CHECK(!w.showing("ai.panel"));
        whole(1.0f,"at "+juce::String(size.x)+" x "+juce::String(size.y));
    }
}
// Switching side units keeps the stack whole and the house unit on its own fold, which its triangle still sets: the AI
// unit, the LIBRARY, the AI unit again, then the lever to ADVANCED, which closes the AI unit, and back to RACK.
void tilePopupEditorRetirement()
{
    std::cout<<"SECTION tile_popup_editor_retirement"<<std::endl;
    auto storage=std::make_unique<WindowRig>();auto& w=*storage;
    SoundDocument sound;std::uint64_t edit=0;CHECK(w.p.trySoundSnapshot(sound,edit));
    RackOperation op;op.design=airack::test::frozenTileArrayRackDesign();op.newInstance=juce::Uuid();op.allowSlotReuse=true;op.allocation={{0,0}};op.destination={RackRole::synth,masterChain,0};
    CHECK(w.p.applyRackOperation(op,edit)==RackCandidateStatus::ready);pump(200);
    auto* stack=w.stack();auto* unit=stack?stack->unit(op.newInstance):nullptr;auto* face=unit?unit->face():nullptr;CHECK(face);if(!face)return;
    face->setOpen(true,false);auto* import=face->revealed().button("import");CHECK(import);if(!import)return;
    auto* handler=import->getAccessibilityHandler();CHECK(handler&&handler->getActions().invoke(juce::AccessibilityActionType::press));pump(100);
    auto* popup=widget<studio::DrumSourcePopup>(*w.editor,"drum.source.popup");CHECK(popup&&popup->isVisible());if(!popup)return;
    popup->close();pump(100);CHECK(!popup->isVisible());
    juce::Component::SafePointer<studio::DrumSourcePopup> alive(popup);int afterPopupDestruction=0;
    const auto original=stack->onDrumSourceClose;
    // Observe the real retirement callback. Protect the known invalid call so
    // the old-source regression records its causal assertion rather than UB.
    stack->onDrumSourceClose=[&afterPopupDestruction,alive,original](juce::Uuid id){if(!alive){++afterPopupDestruction;return;}if(original)original(id);};
    w.editor.reset();pump(100);CHECK(!alive);CHECK(afterPopupDestruction==0);
}
void tileAiManualStatus()
{
    std::cout<<"SECTION tile_ai_manual_status"<<std::endl;
    auto storage=std::make_unique<WindowRig>();auto& w=*storage;auto& e=*w.editor;click(e,"ai.toggle");
    auto* status=widget<juce::Label>(e,"ai.status");auto* screen=screenOf(e,"ai.screen");CHECK(status&&screen);if(!status||!screen)return;
    pump(200);CHECK(!status->getText().contains("Drum AI"));
    SoundDocument sound;std::uint64_t edit=0;CHECK(w.p.trySoundSnapshot(sound,edit));
    RackOperation op;op.design=airack::test::frozenTileArrayRackDesign();op.newInstance=juce::Uuid();op.allowSlotReuse=true;op.allocation={{0,0}};op.destination={RackRole::synth,masterChain,0};
    CHECK(w.p.applyRackOperation(op,edit)==RackCandidateStatus::ready);pump(200);
    CHECK(w.p.trySoundSnapshot(sound,edit));CHECK(sourceModule(sound,0)==NativeModule::drum);
    CHECK(wait("Offline account status with Tile",[&]{return status->getText().isNotEmpty()&&!status->getText().contains("Drum AI editing unavailable");}));
    std::cout<<"DRUM_AI_STATUS "<<status->getText()<<" synthCount="<<int(sound.layout.synthCount);for(unsigned bank=0;bank<4;++bank)std::cout<<" bank="<<bank<<" use="<<int(sound.layout.synthUse[bank])<<" source="<<int(sourceModule(sound,std::uint8_t(bank)));std::cout<<std::endl;
    CHECK(!screen->shown.status.contains("Drum AI editing unavailable"));
    std::shared_ptr<const RackCatalogSnapshot> catalog;CHECK(w.p.captureAvailableRackCatalog(catalog));CHECK(catalog);
    if(catalog)CHECK(std::count_if(catalog->entries.begin(),catalog->entries.end(),[](const auto& entry){return entry.definition.id==airack::test::frozenTileArrayRackDesign().id;})==1);
    DrumPadTarget target;DrumCommitError error;CHECK(w.p.captureDrumPerformanceTarget(op.newInstance,0,15,target,error));CHECK(w.p.enqueueDrumPad(target,96));
    CHECK(screen->shown.spoken==status->getText());
    CHECK(status->getText().containsIgnoreCase("offline")||status->getText().containsIgnoreCase("connect"));
    CHECK(w.p.trySoundSnapshot(sound,edit));RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=op.newInstance;
    CHECK(w.p.applyRackOperation(remove,edit)==RackCandidateStatus::ready);pump(200);CHECK(!status->getText().contains("Drum AI"));
    // Destroy the editor before JUCE releases every deferred removed-rack message.
    // The retired maker's Host callbacks must tolerate the dead/closing receiver.
    storage.reset();
}
void windowSideSwitch()
{
    std::cout<<"SECTION window_side_switch\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* ai=widget<studio::SkinButton>(e,"ai.toggle");
    auto* add=widget<juce::Button>(e,"rack.add");
    auto* lever=widget<studio::Lever>(e,"rack.toggle");
    auto* stack=w.stack();
    auto* house=w.house();
    if(ai==nullptr||add==nullptr||lever==nullptr||stack==nullptr||house==nullptr)return;
    drawnRacks(w.p);
    CHECK(wait("the drawn racks' units",[&]{return w.rack(2)!=nullptr;}));
    const auto whole=[&]{return !stack->narrow()&&!stack->houseOpen()&&stack->getWidth()==1547&&!house->showsStrip()&&!house->isFolded();};
    click(*ai);
    CHECK(w.showing("ai.panel")&&whole());
    pressRail(house->rail);pump();
    CHECK(house->isFolded()&&house->showsStrip()&&!stack->narrow()&&!stack->houseOpen());
    pressRail(house->rail);pump();
    CHECK(whole());
    click(*add);
    CHECK(w.inLibrary()&&whole());
    click(*ai);
    CHECK(w.showing("ai.panel")&&whole());
    click(*lever);
    CHECK(w.showing("pages")&&!w.showing("ai.panel"));
    click(*lever);
    CHECK(!w.showing("ai.panel")&&whole());
}
// The prototype's side slot (stage B lines 57 and 58) in the window: AI opens the AI unit right of the rack under the
// session bar, and the strip narrows beside it, keeping EXPORT MIDI and mapping the pattern onto the narrow overview.
void sideStageLayout()
{
    std::cout<<"SECTION side_stage_layout\n";
    WindowRig w;
    auto& e=*w.editor;
    const auto& m=w.model();
    const auto& L=m.layout;
    auto* stage=w.stage();
    auto* session=find(e,"session");
    auto* strip=find(e,"strip");
    auto* unit=find(e,"ai.panel");
    auto* ai=widget<juce::Button>(e,"ai.toggle");
    auto* wideExport=widget<studio::SkinButton>(e,"strip.export");
    auto* wideOverview=widget<studio::Overview>(e,"strip.overview");
    if(stage==nullptr||session==nullptr||strip==nullptr||unit==nullptr||ai==nullptr||wideExport==nullptr||wideOverview==nullptr)return;
    ProjectDocument d;std::uint64_t revision=0,reset=0;
    CHECK(w.p.tryProjectSnapshot(d,revision,reset)&&w.p.applyPatternEdit(mockupPattern(m),revision));
    pump();
    CHECK(!unit->isShowing()&&strip->getWidth()==stage->getWidth()&&strip->getBottom()==stage->getHeight()&&session->getWidth()==stage->getWidth());
    click(*ai);
    CHECK(unit->isShowing()&&unit->getParentComponent()==stage&&unit->getPosition()==juce::Point<int>((int)L["side"]["x"],(int)L["side"]["y"])
          &&unit->getWidth()==401&&unit->getHeight()==852&&unit->getRight()==session->getRight()&&unit->getY()==session->getBottom()+4
          &&unit->getBottom()<=stage->getHeight());
    CHECK(strip->getWidth()==(int)L["strip_narrow"]["width"]&&strip->getHeight()==104&&strip->getRight()+3==unit->getX()&&strip->getBottom()==stage->getHeight());
    std::cout<<"  the AI unit at "<<unit->getPosition().toString()<<", "<<unit->getWidth()<<" x "<<unit->getHeight()<<"; the strip "<<strip->getWidth()<<" px\n";
    auto* narrowExport=widget<studio::SkinButton>(e,"strip.export");
    auto* narrowOverview=widget<studio::Overview>(e,"strip.overview");
    if(narrowExport==nullptr||narrowOverview==nullptr)return;
    const auto cut=(float)((int)L["strip_narrow"]["cut"][1]-(int)L["strip_narrow"]["cut"][0]);
    int last=-1;
    for(const auto& [column,row]:narrowOverview->cells)last=std::max(last,column);
    CHECK(narrowExport!=wideExport&&narrowExport->isShowing()&&std::abs(narrowExport->element.rect.getX()-(wideExport->element.rect.getX()-cut))<0.01f);
    CHECK(m.overviewNarrow.columns==(int)L["strip_narrow"]["columns"]&&narrowOverview->getWidth()==wideOverview->getWidth()-(int)cut);
    CHECK(narrowOverview->cells==studio::overviewCells(m.mockup.notes,m.overviewNarrow,4,4,4)&&narrowOverview->cells.size()==64&&last==51);
    std::cout<<"  the narrowed overview: "<<narrowOverview->cells.size()<<" lit cells, last column "<<last<<'\n';
}
// Gallery occupies the middle workspace and darkens AI; AI returns its narrow composition. Cancel restores prior state.
void sideSwaps()
{
    std::cout<<"SECTION side_swaps\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* stage=w.stage();
    auto* session=find(e,"session");
    auto* strip=find(e,"strip");
    auto* unit=find(e,"ai.panel");
    auto* library=find(e,"library.panel");
    auto* ai=widget<studio::SkinButton>(e,"ai.toggle");
    auto* add=widget<juce::Button>(e,"rack.add");
    if(stage==nullptr||session==nullptr||strip==nullptr||unit==nullptr||library==nullptr||ai==nullptr||add==nullptr)return;
    const auto before=w.revisions();
    click(*ai);
    CHECK(unit->isShowing()&&ai->lit());
    click(*add);
    CHECK(library->isShowing()&&!unit->isShowing()&&!ai->lit()&&strip->getWidth()==1547&&library->getParentComponent()==stage
          &&library->getPosition()==juce::Point<int>(0,int(w.model().layout["advanced"]["y"]))&&library->getRight()==session->getRight()&&library->getBottom()==strip->getY());
    click(*ai);
    CHECK(unit->isShowing()&&!library->isShowing()&&ai->lit()&&strip->getWidth()==1143);
    click(*ai);
    CHECK(!unit->isShowing()&&!ai->lit()&&strip->getWidth()==1547);
    click(*add);
    click(e,"library.cancel");
    CHECK(!library->isShowing()&&!unit->isShowing()&&!ai->lit()&&strip->getWidth()==1547&&w.showing("strip.export"));
    CHECK(w.revisions()==before);
}
// Stage B line 76: with a side unit open, Tab runs from the narrowed strip through the house unit, the rack's unit and
// + ADD RACK into the side unit's focus order, and ends there.
void sideTabOrder()
{
    std::cout<<"SECTION side_tab_order\n";
    for(const char* unit:{"ai","library"})
    {
        WindowRig w;
        auto& e=*w.editor;
        auto* house=w.house();
        if(house==nullptr)return;
        placeRack(w.p,0,{},{{0,0},{1,0}},true);
        CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
        click(e,juce::String(unit)=="ai"?"ai.toggle":"rack.add");
        if(juce::String(unit)=="library"){
            auto* browser=dynamic_cast<studio::LibraryUnit*>(find(e,"library.panel"));CHECK(browser&&wait("Gallery keyboard rows ready",[&]{browser->refresh();return browser->matchingRows()>0;}));
            juce::KeyboardFocusTraverser traverser;auto* c=find(e,"strip.fold");juce::StringArray got;
            for(int n=0;c&&n<128;++n){const auto id=c->getComponentID();CHECK(!got.contains(id));got.add(id);c=traverser.getNextComponent(c);}
            CHECK(c==nullptr&&got.contains("library.search")&&got.contains("library.cancel")&&got.contains("library.gallery"));
            CHECK(!got.contains("rack.add")&&!got.contains("house.fold")&&!got.contains("rack.operations"));continue;
        }
        juce::StringArray want;
        for(const auto& id:w.model().focusOrder.at("strip"))want.add("strip."+id);
        want.add("house.fold");
        for(const auto& id:house->unit.focusOrder)want.add("house."+id);
        want.addArray({"rack.fold","face.air","face.bite","face.bloom","face.attack","face.release","face.grain","face.programmer","rack.operations"});
        want.add("rack.add");
        for(const auto& id:w.model().focusOrder.at(unit))want.add(juce::String(unit)+"."+id);
        const bool aiSide=juce::String(unit)=="ai";
        if(aiSide)want.add("ai.mcp");
        auto* mcp=find(e,"ai.mcp");
        CHECK(mcp!=nullptr&&mcp->getWantsKeyboardFocus()&&mcp->isShowing()==aiSide);
        std::cout<<"MCP_FOCUS_ACCESS context=side_tab_order unit="<<unit<<" id="<<(mcp?mcp->getComponentID():juce::String("<null>"))
                 <<" showing="<<(mcp&&mcp->isShowing())<<" wantsKeyboardFocus="<<(mcp&&mcp->getWantsKeyboardFocus())<<'\n';
        auto stop=[](juce::Component* x){ const auto id=x->getComponentID();return id=="ai.mcp"||id.startsWith("house.")||id.startsWith("face.")||(id.startsWith("rack.")&&id!="rack.toggle")?id:x->getProperties()["element"].toString(); };
        juce::KeyboardFocusTraverser traverser;
        juce::Component* c=widget<studio::Disclosure>(e,"strip.fold");
        juce::StringArray got;
        for(int i=0;i<want.size()&&c!=nullptr;++i){got.add(stop(c));c=traverser.getNextComponent(c);}
        std::cout<<"FOCUS_RESIDUAL context=side_tab_order unit="<<unit<<" prefix_equal="<<(got==want)
                 <<" id="<<(c?c->getComponentID():juce::String("<null>"))<<" name="<<(c?c->getName():juce::String("<null>"))
                 <<" showing="<<(c&&c->isShowing())<<" wantsKeyboardFocus="<<(c&&c->getWantsKeyboardFocus())<<'\n';
        CHECK(got==want&&c==nullptr);
        if(got!=want)std::cout<<"  "<<unit<<": "<<got.joinIntoString(", ")<<'\n';
    }
}
// Stage B line 77: the gap under the session bar and the seam beside the narrowed strip show the drawn seam.
void sideSeam()
{
    std::cout<<"SECTION side_seam\n";
    WindowRig w;
    auto& e=*w.editor;
    const auto& L=w.model().layout;
    e.setSize((int)L["stage"][0],(int)L["stage"][1]);pump();
    click(e,"ai.toggle");
    const auto image=snap(e);
    const auto& gap=L["gap_bgr"];
    const auto seam=juce::Colour((juce::uint8)(int)gap[2],(juce::uint8)(int)gap[1],(juce::uint8)(int)gap[0]);
    const auto sx=(int)L["side"]["x"],sy=(int)L["side"]["y"];
    const auto above=image.getPixelAt(sx+100,sy-2),beside=image.getPixelAt(sx-2,(int)L["strip"]["y"]+50);
    CHECK(above==seam&&beside==seam&&seam.getBrightness()<0.05f);
    std::cout<<"  the seam "<<seam.toDisplayString(false)<<": above "<<above.toDisplayString(false)<<", beside "<<beside.toDisplayString(false)<<'\n';
}
// A recall between opening ASSIGN and picking a target cancels the pick with today's sentence; the next pick applies.
void windowAssignRecall()
{
    std::cout<<"SECTION window_assign_recall\n";
    WindowRig w;
    auto& e=*w.editor;
    auto* house=w.house();
    auto* status=widget<juce::Label>(e,"status");
    if(house==nullptr||status==nullptr)return;
    auto* assign5=house->unit.button("assign.5");
    CHECK(assign5!=nullptr);
    if(assign5==nullptr)return;
    auto& ed=house->editor;
    press(*assign5);
    CHECK(ed.isVisible()&&ed.macro==5&&ed.target.getNumItems()>1);
    const auto item=ed.target.getItemText(1);
    const auto catalog=parameterRegistry();
    std::size_t first=0;
    while(first<catalog.size()&&!(catalog[first].kind==ParameterKind::continuous&&(first==0||first>=9)))++first;
    const auto target=catalog[first].stateKey;
    CHECK(restore(w.p,state(document(w.p))));
    CHECK(pick(ed,item));
    CHECK(document(w.p).macros[4]==MacroBinding{}&&ed.target.getSelectedId()==1);
    CHECK(status->getText()=="Sound recalled; pending selection cancelled. Select the edit again.");
    CHECK(pick(ed,item));
    CHECK(document(w.p).macros[4].targetKey==target&&status->getText()=="Macro assignment complete");
}
void windowWithoutSkin()
{
    std::cout<<"SECTION window_without_skin\n";
    InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
    InstrumentEditor e(p,nullptr);
    auto* line=widget<juce::Label>(e,"skin.error");
    CHECK(e.getWidth()==1080&&e.getHeight()==680&&find(e,"stage")==nullptr&&find(e,"session")==nullptr&&find(e,"strip")==nullptr);
    CHECK(line!=nullptr&&line->isVisible()&&e.getLocalBounds().contains(line->getBounds())&&line->getText().startsWith("Studio skin failed to load"));
    e.setSize(1600,1200);
    CHECK(e.createComponentSnapshot(e.getLocalBounds()).isValid());
}
// With --images <dir>: the window at 1080 x 680 in the twenty-three states the user reviews, over the drawn mock-up's
// session (its preset path and host tempo) and pattern; the unfolded unit holds its own drawn pattern, INTERNAL with
// the host playing or EXT MIDI with it stopped, and the drawn house unit its racks, macros and values, which five of
// the PROGRAMMER's pages show too, the stack's first rack selected. The processor is ready before the editor opens, as
// when a host opens a project, so no snapshot shows a cancelled edit or a recall.
void reviewSnapshots()
{
    if(output==juce::File{})return;
    std::cout<<"SECTION review_snapshots\n";
    enum class Before { nothing, failedLoad, syncedWithoutHost, playingUnit, stoppedUnit, drawnHouse };
    struct Shot
    {
        Playhead head;
        // On the heap: a Debug build gives each block's Shot its own slot in this function's stack frame.
        std::unique_ptr<InstrumentProcessor> owned=std::make_unique<InstrumentProcessor>(WindowRig::offline(), airack::testing::isolatedRackRuntime());
        InstrumentProcessor& p=*owned;
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        explicit Shot(Before before=Before::nothing)
        {
            const auto& m=*studio::studioSkin();
            CHECK(blankInstance(p));
            if(before!=Before::syncedWithoutHost){head.bpm=m.mockup.bpm;p.setPlayHead(&head);}
            if(before==Before::failedLoad){const char junk[]{1,2,3,4,5};p.setStateInformation(junk,5);}
            if(before==Before::syncedWithoutHost)
            {
                auto synced=document(p);
                physical(synced,"synth.1.lfo.1.synced",1);physical(synced,"synth.1.modulation.1.enabled",1);
                physical(synced,"synth.1.modulation.1.source",3);physical(synced,"synth.1.modulation.1.target",1);
                CHECK(restore(p,state(synced)));
            }
            if(before==Before::drawnHouse){drawnRacks(p);drawnMacros(p);drawnValues(p,"unit");}
            p.prepareToPlay(48000,64);
            const bool unit=before==Before::playingUnit||before==Before::stoppedUnit;
            ProjectDocument d;std::uint64_t revision=0,reset=0;
            CHECK(p.tryProjectSnapshot(d,revision,reset)&&p.applyPatternEdit(unit?drawnPattern(m):mockupPattern(m),revision));
            if(before==Before::playingUnit)
            {
                CHECK(p.tryProjectSnapshot(d,revision,reset)&&p.setPlaybackMode(PlaybackMode::internalPattern,revision));
                head.playing=true;head.ppq=(double)m.patternMockup["playhead_beats"];
            }
            juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),0);
            p.processBlock(audio,midi);
            CHECK(p.tempoUnavailable()==(before==Before::syncedWithoutHost)&&p.stateLoadFailed()==(before==Before::failedLoad));
            editor.reset(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);
            if(auto* e=widget<juce::TextEditor>(*editor,"preset.path"))e->setText(m.mockup.path);else CHECK(false);
            pump(60);
        }
        ~Shot() { editor.reset();pump();p.setPlayHead(nullptr);p.releaseResources(); }
        void press(const char* id) { if(auto* b=widget<juce::Button>(*editor,id))click(*b,60); else CHECK(false); }
        void unfold() { if(auto* fold=widget<studio::Disclosure>(*editor,"strip.fold")){fold->press();pump(60);} else CHECK(false); }
        // + ADD RACK as nothing points at it: the desktop's pointer may rest over the window, and a press flashes it down.
        void write(const char* name)
        {
            if(auto* add=widget<juce::Button>(*editor,"rack.add"))add->setState(juce::Button::buttonNormal);
            CHECK(editor->getWidth()==1080&&editor->getHeight()==680&&writePng(output.getChildFile(name),snap(*editor)));
        }
        studio::HouseUnit* house() { return dynamic_cast<studio::HouseUnit*>(find(*editor,"house.panel")); }
        RackStack* stack() { auto* view=find(*editor,"rack.stack");return view!=nullptr?dynamic_cast<RackStack*>(view->getParentComponent()):nullptr; }
        RackStackUnit* rack(int n) { return dynamic_cast<RackStackUnit*>(find(*editor,"rack.unit."+juce::String(n))); }
    };
    {
        Shot s;
        s.write("window-default.png");
        s.press("rack.toggle");
        s.write("window-advanced.png");
    }
    {
        // The PROGRAMMER's pages over the drawn racks; a press and release on SYNTH 1's CUTOFF fills the value line.
        Shot s(Before::drawnHouse);
        s.press("rack.toggle");
        auto* unit=dynamic_cast<studio::ProgrammerUnit*>(find(*s.editor,"pages"));
        CHECK(unit!=nullptr);
        if(unit!=nullptr)
        {
            s.write("window-programmer-global.png");
            unit->showPage("synth1");unit->showSubpage("analog");
            if(auto* cutoff=unit->knob(39))
            {
                const auto at=cutoff->getLocalBounds().getCentre().toFloat();
                cutoff->mouseDown(event(*cutoff,at,juce::ModifierKeys::leftButtonModifier,1,false));
                cutoff->mouseUp(event(*cutoff,at,{},1,false));
            }
            else CHECK(false);
            pump(60);
            s.write("window-programmer-synth1-analog.png");
            unit->showSubpage("routes");pump(60);
            s.write("window-programmer-synth1-routes.png");
            // FX 1 opened from FX 2: pressing the shown FX N again steps its pages.
            unit->showPage("effects");unit->showSubpage("fx2");unit->showSubpage("fx1");pump(60);
            s.write("window-programmer-effects-fx1.png");
            unit->showPage("layout");pump(60);
            s.write("window-programmer-layout.png");
        }
    }
    {
        Shot s;
        s.press("ai.toggle");
        s.write("window-ai.png");
    }
    {
        Shot s;
        s.press("rack.add");
        if(auto* library=dynamic_cast<studio::LibraryUnit*>(find(*s.editor,"library.panel"))){CHECK(wait("Gallery inventory",[&]{library->refresh();return library->matchingRows()>0;}));pump(60);}
        else CHECK(false);
        s.write("window-library.png");
    }
    {
        Shot s(Before::drawnHouse);
        s.write("window-drawn.png");
        if(auto* house=s.house())
        {
            pressRail(house->rail);pump(60);
            s.write("window-folded.png");
            pressRail(house->rail);pump(60);
            auto* assign5=house->unit.button("assign.5");
            CHECK(assign5!=nullptr);
            if(assign5!=nullptr)
            {
                press(*assign5);
                CHECK(pick(house->editor,"BLOOM   Ember Reed"));
                house->editor.depth.setValue(25.0,juce::sendNotificationSync);pump(60);
                CHECK(house->unit.tape("macro-tape.5")->text()=="BLOOM +25");
                s.write("window-assign.png");
            }
        }
        else CHECK(false);
    }
    {
        Shot s(Before::drawnHouse);
        s.press("rack.add");
        s.write("window-drawn-library.png");
    }
    // The drawn racks in the stack: Glass Current and Thread Press folded to their strips and scrolled to, Ember Reed's
    // drawer open, the stack scrolled to its foot, and the stack narrowed beside the AI unit.
    {
        Shot s(Before::drawnHouse);
        auto* stack=s.stack();
        auto* glass=s.rack(1);
        auto* thread=s.rack(2);
        CHECK(stack!=nullptr&&glass!=nullptr&&thread!=nullptr);
        if(stack!=nullptr&&glass!=nullptr&&thread!=nullptr)
        {
            pressRail(glass->fold);pressRail(thread->fold);
            stack->viewport().setViewPosition(0,stack->body().getHeight());pump(60);
            s.write("window-drawn-folded.png");
        }
    }
    {
        Shot s(Before::drawnHouse);
        if(auto* ember=s.rack(0)){click(ember->operations,60);s.write("window-drawer.png");}
        else CHECK(false);
    }
    {
        Shot s(Before::drawnHouse);
        if(auto* stack=s.stack()){stack->viewport().setViewPosition(0,stack->body().getHeight());pump(60);s.write("window-drawn-foot.png");}
        else CHECK(false);
    }
    {
        Shot s(Before::drawnHouse);
        s.press("ai.toggle");
        s.write("window-drawn-narrow.png");
    }
    {
        Shot s;
        s.unfold();
        s.write("window-unfolded.png");
    }
    {
        Shot s(Before::playingUnit);
        s.unfold();
        auto* roll=widget<studio::Roll>(*s.editor,"pattern.roll");
        auto* start=widget<studio::Knob>(*s.editor,"pattern.note.start");
        if(roll!=nullptr&&start!=nullptr)
        {
            clickAt(*roll,roll->noteRect((int)studio::studioSkin()->patternMockup["selected"]).getCentre());
            juce::Component::unfocusAllComponents();pump(60);
            s.write("window-pattern.png");
            start->openEntry();pump(60);
            CHECK(start->entry.isVisible());
            s.write("pattern-entry.png");
        }
    }
    {
        Shot s(Before::stoppedUnit);
        s.unfold();
        auto* bars=widget<studio::Knob>(*s.editor,"pattern.bars");
        auto* status=widget<juce::Label>(*s.editor,"pattern.status");
        if(bars!=nullptr&&status!=nullptr)
        {
            bars->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
            juce::Component::unfocusAllComponents();pump(60);
            CHECK(status->getText().startsWith("Invalid phrase size"));
            s.write("pattern-states.png");
        }
    }
    {
        Shot s(Before::failedLoad);
        s.write("session-load-error.png");
    }
    {
        Shot s(Before::syncedWithoutHost);
        s.write("session-no-tempo.png");
    }
}
// The AI unit and the LIBRARY (the prototype's stage B, SelfTest::sideUnits).
Rect lensOf(const studio::Element& e) { return Rect(2*e.lensR,2*e.lensR).withCentre(e.led); }
juce::String shownText(const studio::Screen& s,const juce::String& key) { return s.texts.count(key)?s.texts.at(key).shown:juce::String("(none)"); }
struct SideDrawn
{
    studio::ScreenShown screen;
    juce::String account,accountState,linked,linkedState,model;
};
// A drawn state of the side units (elements.json's mockup.side) as their units would show it: the prototype's
// setSideState over its defaults, with the titles and sentences its screen chose for that state. The drawings'
// CREATE RACK and REFINE RACK states show as CREATE SOUND's, without their rack fields, target and SAVE DESIGN.
SideDrawn drawnSide(const studio::Model& m,const juce::String& key)
{
    const auto st=m.sideStates[juce::Identifier(key)];
    SideDrawn d;
    auto& s=d.screen;
    s.page=st["page"].toString();
    d.model=st["model"].toString();
    if(s.page=="library")
    {
        for(const auto& row:*st["rows"].getArray())
        {
            juce::StringArray cells;
            for(const auto& c:*row.getArray())cells.add(c.toString());
            s.rows.push_back(cells);
        }
        s.chosen=(int)st["chosen"];
        s.place=st["place"].toString();
        s.banks=st["banks"].toString();
        s.review=st["review"].toString();
        return d;
    }
    const auto account=st["account"].toString();
    const auto linked=(bool)st["linked"];
    const auto heading=st["heading"].toString();
    d.account=account=="offline"?"amber":account=="connected"?"green":"";
    d.accountState=account=="offline"?"companion offline":account=="connected"?"connected":"signed out";
    d.linked=linked?"lit":"";
    d.linkedState=linked?"linked: "+heading.fromFirstOccurrenceOf(": ",false,false):juce::String("not linked");
    std::map<juce::String,juce::String> context{{"KEY","D"},{"SCALE","MINOR"},{"ROLE","LEAD"},{"BPM","120"}};
    if(auto* o=st["context"].getDynamicObject())
        for(const auto& p:o->getProperties())context[p.name.toString()]=p.value.toString();
    for(const auto* f:{"KEY","SCALE","ROLE","BPM"})s.context.push_back({f,context[f]});
    s.options={{"ai.allowMacros",0},{"ai.allowReuse",0},{"ai.scope",0},{"ai.requirePattern",0}};
    if(auto* o=st["options"].getDynamicObject())
        for(const auto& p:o->getProperties())
            if(const auto option="ai."+p.name.toString();s.options.count(option))
                s.options[option]=p.value.isBool()?((bool)p.value?1:0):(int)p.value;
    if(s.options["ai.scope"]==0)s.dim.add("ai.requirePattern");
    s.prompt=st["prompt"].toString();
    s.promptFocus=st["focus"].toString()=="prompt";
    if(auto* changes=st["changes"].getArray())
        for(const auto& c:*changes)s.changes.push_back({c[0].toString(),c[1].toString(),c[2].isVoid()?juce::String():c[2].toString(),c[2].isVoid()});
    const auto proposal=!s.changes.empty();
    s.title=proposal?"PROPOSAL READY":"NO PROPOSAL";
    s.status=proposal?"LIVE SOUND UNCHANGED UNTIL APPLY":"READY TO GENERATE";
    const auto page=m.screens["pages"][juce::Identifier(s.page)];
    s.heading=heading.isNotEmpty()?heading:page["heading"]["text"].toString();
    if(auto* body=st["body"].getArray()) { for(const auto& b:*body)s.body.add(b.toString()); }
    else if(auto* slots=page["body"].getArray()) { for(const auto& b:*slots)s.body.add(b["text"].toString()); }
    s.button=page["button"]["text"].toString();
    if(auto* models=st["models"].getArray())for(const auto& x:*models)s.list.add(x.toString());
    s.chosen=st["chosen"].isVoid()?-1:(int)st["chosen"];
    s.highlight=(int)st.getProperty("highlight",-1);
    s.leaveEnabled=linked;
    if(auto* peers=st["peers"].getArray())
        for(const auto& p:*peers)s.peers.push_back({p[0].toString(),p[1].toString(),p[2].toString(),{},(bool)p[3]});
    return d;
}
// The AI unit and the LIBRARY side by side on the desktop, counting each control's presses and showing what their
// units would show them.
struct SideRig final:juce::Component
{
    const studio::Model& model=*studio::studioSkin();
    studio::SkinView ai{model,"ai"},library{model,"library"};
    std::map<juce::String,int> commands;
    studio::ScreenShown shown;
    // What the screens reported: presses, committed entries and Escapes.
    std::vector<std::pair<juce::String,int>> reported;
    std::vector<std::pair<juce::String,juce::String>> entered;
    int escapes=0;
    SideRig()
    {
        addAndMakeVisible(ai);addAndMakeVisible(library);
        library.setTopLeftPosition(ai.getRight()+20,0);
        setSize(library.getRight(),std::max(ai.getHeight(),library.getHeight()));
        for(auto* v:{&ai,&library})
        {
            for(auto& b:v->buttons) b->onClick=[this,id=b->element.id]{++commands[id];};
            for(auto& t:v->targets) t->onClick=[this,id=t->element.id]{++commands[id];};
            if(v->readout) v->readout->onClick=[this,id=v->readout->element.id]{++commands[id];};
            v->screen->onCommand=[this](const juce::String& command,int value){reported.push_back({command,value});};
            v->screen->onEntry=[this](const juce::String& command,const juce::String& text){entered.push_back({command,text});};
            v->screen->onEscape=[this]{++escapes;};
        }
        addToDesktop(0);setVisible(true);
    }
    ~SideRig() override { removeFromDesktop(); }
    void show(const juce::String& key) { show(drawnSide(model,key)); }
    void show(const SideDrawn& d)
    {
        shown=d.screen;
        if(shown.page=="library"){library.screen->show(shown);return;}
        ai.screen->show(shown);
        ai.get<studio::Target>("ai.account")->show(d.account,d.accountState);
        ai.get<studio::Target>("ai.linked")->show(d.linked,d.linkedState);
        ai.readout->show(d.model);
        ai.repaint();
    }
};
struct DrawnRun { juce::String text; Rect ink; };
// The texts side_units.py read off a drawing (the skin's side-screens.json); the account copies scaled onto the AI
// unit's glass.
std::vector<DrawnRun> drawnRuns(const studio::Model& m,const juce::String& key)
{
    static const auto doc=juce::JSON::parse(juce::File(RACKSYNTH_WORKSPACE).getChildFile("docs/design/racks/2026-10-09-studio-rack/window/skin/side-screens.json"));
    const auto sc=doc[juce::Identifier(key)];
    const auto num=[](const juce::var& v){return (float)(double)v;};
    auto fx=1.0f,fy=1.0f;
    if(key.startsWith("account"))
    {
        const auto aiGlass=m.screens["glass"]["ai"];
        fx=num(aiGlass[2])/num(sc["glass"][0]);
        fy=num(aiGlass[3])/num(sc["glass"][1]);
    }
    const auto box=[&](const juce::var& b){return Rect::leftTopRightBottom(num(b[0])*fx,num(b[1])*fy,num(b[2])*fx,num(b[3])*fy);};
    std::vector<DrawnRun> out;
    if(auto* rows=sc["rows"].getArray())
        for(const auto& e:*rows)
        {
            if(auto* runs=e["runs"].getArray())
                for(const auto& r:*runs) if(!r["text"].toString().startsWith("<")) out.push_back({r["text"].toString(),box(r["ink"])});
            if(auto* lines=e["lines"].getArray()) for(const auto& l:*lines) out.push_back({l["text"].toString(),box(l["ink"])});
            if(auto* buttons=e["buttons"].getArray()) for(const auto& b:*buttons) out.push_back({b["text"].toString(),box(b["ink"])});
            if(e.hasProperty("status")) out.push_back({e["status"]["text"].toString(),box(e["status"]["ink"])});
            if(e["kind"].toString()=="button") out.push_back({e["text"].toString(),box(e["ink"])});
        }
    return out;
}
// The side units and the narrowed strip at 1x in the prototype's drawn states, against its native snapshots: the AI
// unit and the LIBRARY where the stage draws them, and a request in flight (section 25). The snapshots show the
// mode row, a target and SAVE DESIGN, and the LIBRARY's tabs, three rows, LOAD FILE and REVIEW, where the plates are
// painted out or the list now shows five rows; those regions are left out.
void sideMatchesPrototype()
{
    std::cout<<"SECTION side_matches_prototype\n";
    const auto native=juce::File(RACKSYNTH_WORKSPACE).getChildFile("docs/design/racks/2026-10-09-studio-rack/window/skin/compare/native");
    const auto load=[&](const char* name){return juce::ImageFileFormat::loadFrom(native.getChildFile(name));};
    const auto drawn=[](juce::Component& c){return c.createComponentSnapshot(c.getLocalBounds(),true,1.0f);};
    const juce::Rectangle<int> modeRow{45,91,303,109},header{12,207,376,52},saveDesign{220,390,160,40},context{12,340,376,30},list{13,98,374,252},
                               libraryButtons{73,572,242,104};
    {
        SideRig rig;
        juce::Component::unfocusAllComponents();
        rig.show("ai");
        const auto stageAi=load("stage-ai.png");
        CHECK(stageAi.getWidth()==1547&&stageAi.getHeight()==965);
        if(stageAi.isValid())matchesPrototype(drawn(rig.ai),stageAi.getClippedImage({1146,111,401,852}),"AI unit",{modeRow,header,saveDesign});
        rig.show("library");
        const auto stageLibrary=load("stage-library.png");
        CHECK(stageLibrary.getWidth()==1547&&stageLibrary.getHeight()==965);
        if(stageLibrary.isValid())matchesPrototype(drawn(rig.library),stageLibrary.getClippedImage({1147,111,400,854}),"LIBRARY",{list,libraryButtons});
        auto pending=drawnSide(rig.model,"create");
        auto& p=pending.screen;
        p.page="proposal";p.title="GENERATING";p.action="CANCEL";p.status="WAITING FOR THE COMPANION";
        rig.show(pending);
        const auto inFlight=load("side-pending.png");
        CHECK(inFlight.getWidth()==401&&inFlight.getHeight()==852);
        if(inFlight.isValid())matchesPrototype(drawn(rig.ai),inFlight,"request in flight",{modeRow,header,context});
    }
    {
        StripRig rig;
        rig.pattern(mockupPattern(rig.model()));
        rig.strip->setNarrow(true);
        CHECK(rig.shown().text=="4 BARS  4/4  1/16  SWING 0%");
        const auto narrow=load("strip-narrow.png");
        CHECK(narrow.getWidth()==1143&&narrow.getHeight()==104);
        if(narrow.isValid())matchesPrototype(drawn(*rig.strip),narrow,"narrowed strip");
    }
}
// With --images <dir>: side-drawn.png, the seven drawn states as the side units draw them, and side-keyboard.png, the
// create page with the keyboard's cursor on SCOPE.
void sideSnapshots()
{
    if(output==juce::File{})return;
    std::cout<<"SECTION side_snapshots\n";
    SideRig rig;
    juce::Component::unfocusAllComponents();
    const int gap=20;
    juce::Image sheet(juce::Image::ARGB,6*(rig.ai.getWidth()+gap)+rig.library.getWidth(),std::max(rig.ai.getHeight(),rig.library.getHeight()),true);
    {
        juce::Graphics g(sheet);
        g.fillAll(juce::Colour(0xff020202));
        int x=0;
        for(const auto* key:{"ai","create","offline","account0","account1","account2","library"})
        {
            rig.show(key);
            auto& v=juce::String(key)=="library"?rig.library:rig.ai;
            g.drawImageAt(v.createComponentSnapshot(v.getLocalBounds(),true,1.0f),x,0);
            x+=v.getWidth()+gap;
        }
    }
    CHECK(writePng(output.getChildFile("side-drawn.png"),sheet));
    rig.show("create");
    auto& scr=*rig.ai.screen;
    auto& md=*rig.ai.readout;
    md.grabKeyboardFocus();
    CHECK(wait("the model display has the focus",[&]{return md.hasKeyboardFocus(false);}));
    md.moveKeyboardFocusToSibling(true);
    CHECK(wait("the screen has the focus",[&]{return scr.hasKeyboardFocus(false);}));
    CHECK(juce::isPositiveAndBelow(scr.cursor(),(int)scr.hits.size())&&scr.hits[(size_t)scr.cursor()].entry=="ai.prompt");
    scr.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
    CHECK(juce::isPositiveAndBelow(scr.cursor(),(int)scr.hits.size())&&scr.hits[(size_t)scr.cursor()].command=="ai.scope");
    CHECK(writePng(output.getChildFile("side-keyboard.png"),rig.ai.createComponentSnapshot(rig.ai.getLocalBounds(),true,1.0f)));
}
void sideUnitsLoad()
{
    std::cout<<"SECTION side_units_load\n";
    SideRig rig;
    CHECK(rig.model.screenFace!=nullptr&&rig.model.screenFace->getName()=="IBM Plex Sans Condensed");
    CHECK(rig.ai.screen!=nullptr&&rig.ai.readout!=nullptr&&rig.ai.targets.size()==2&&rig.ai.buttons.size()==6);
    CHECK(rig.library.screen!=nullptr&&rig.library.readout==nullptr&&rig.library.targets.empty()&&rig.library.buttons.size()==2);
}
void sideHitRegions()
{
    std::cout<<"SECTION side_hit_regions\n";
    SideRig rig;
    int ok=0,total=0;
    juce::StringArray misses;
    auto probe=[&](bool passed,const juce::String& what){++total;ok+=passed;if(!passed)misses.add(what);};
    for(auto* v:{&rig.ai,&rig.library})
    {
        for(auto& b:v->buttons)
        {
            probe(v->getComponentAt(b->element.rect.getCentre().toInt())==b.get(),b->element.id+" centre");
            probe(v->getComponentAt(Pt(b->element.rect.getX()-7.0f,b->element.rect.getCentreY()).toInt())!=b.get(),b->element.id+" outside");
        }
        for(auto& t:v->targets)
        {
            probe(v->getComponentAt(t->element.led.toInt())==t.get(),t->element.id+" LED");
            probe(v->getComponentAt(t->element.legend.getCentre().toInt())==t.get(),t->element.id+" legend");
            probe(v->getComponentAt(Pt(t->element.hitRect.getRight()+6.0f,t->element.hitRect.getCentreY()).toInt())!=t.get(),t->element.id+" outside");
        }
        probe(v->getComponentAt(v->screen->element.glass.getCentre().toInt())==v->screen.get(),v->screen->element.id+" glass");
    }
    probe(rig.ai.getComponentAt(rig.ai.readout->element.glass.getCentre().toInt())==rig.ai.readout.get(),"ai.model glass");
    CHECK(ok==total&&total==25);
    if(!misses.isEmpty())std::cout<<"missed: "<<misses.joinIntoString(", ")<<'\n';
}
void sidePresses()
{
    std::cout<<"SECTION side_presses\n";
    SideRig rig;
    rig.show("create");
    juce::StringArray wrong;
    for(auto* v:{&rig.ai,&rig.library})
        for(auto& b:v->buttons)
        {
            const auto before=rig.commands[b->element.id];
            press(*b);
            if(rig.commands[b->element.id]!=before+1)wrong.add(b->element.id);
        }
    for(auto& t:rig.ai.targets)
    {
        const auto before=rig.commands[t->element.id];
        clickAt(*t,t->element.legend.getCentre());
        if(rig.commands[t->element.id]!=before+1)wrong.add(t->element.id);
    }
    const auto before=rig.commands["ai.model"];
    clickAt(*rig.ai.readout,rig.ai.readout->element.glass.getCentre());
    if(rig.commands["ai.model"]!=before+1)wrong.add("ai.model");
    CHECK(wrong.isEmpty());
    if(!wrong.isEmpty())std::cout<<"presses: "<<wrong.joinIntoString(", ")<<'\n';
    auto& account=*rig.ai.get<studio::Target>("ai.account");
    auto& display=*rig.ai.readout;
    const auto a=rig.commands["ai.account"],m=rig.commands["ai.model"];
    account.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    account.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    click(account);
    display.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    click(display);
    CHECK(rig.commands["ai.account"]==a+3&&rig.commands["ai.model"]==m+2);
    const auto at=local(account,account.element.legend.getCentre());
    account.mouseDown(event(account,at,juce::ModifierKeys::leftButtonModifier,1,false));
    account.mouseUp(event(account,at+Pt(80.0f,0.0f),{},1,true,at));
    CHECK(rig.commands["ai.account"]==a+3);
}
void sideLeds()
{
    std::cout<<"SECTION side_leds\n";
    SideRig rig;
    // An action's LED draws the lit sprite over its lens.
    auto& generate=*rig.ai.get<studio::SkinButton>("ai.generate");
    const auto lens=[&](const studio::Element& e){return meanLuma(snap(rig.ai),lensOf(e));};
    rig.show("create");
    const auto unlitLed=lens(generate.element);
    generate.litWhen=[]{return true;};generate.showLit();
    const auto litLed=lens(generate.element);
    CHECK(generate.lit()&&litLed>unlitLed+40.0);
    std::cout<<"GENERATE LED: lens luma "<<litLed<<" lit, "<<unlitLed<<" dark\n";
    generate.litWhen=nullptr;generate.showLit();
    auto& account=*rig.ai.get<studio::Target>("ai.account");
    auto& linkedLed=*rig.ai.get<studio::Target>("ai.linked");
    const auto lensRgb=[&](studio::Target& t){return meanChannels(snap(rig.ai),lensOf(t.element));};
    const auto lum=[](std::array<double,3> c){return 0.299*c[0]+0.587*c[1]+0.114*c[2];};
    rig.show("ai");
    const auto green=lensRgb(account);
    rig.show("offline");
    const auto amber=lensRgb(account);
    rig.show("account0");
    const auto dark=lensRgb(account);
    account.show("amber","sign-in expired");
    const auto expired=lensRgb(account);
    account.show({},"signing in");
    const auto signing=lensRgb(account);
    rig.show("account1");
    const auto unlinked=lensRgb(linkedLed);
    rig.show("account2");
    const auto linked=lensRgb(linkedLed);
    CHECK(green[1]>green[0]+25.0&&green[1]>green[2]+25.0&&amber[0]>amber[2]+60.0&&expired[0]>expired[2]+60.0
        &&lum(dark)<lum(green)-40.0&&lum(signing)<lum(green)-40.0&&lum(linked)>lum(unlinked)+30.0);
    std::cout<<"ACCOUNT lens luma: green "<<lum(green)<<", amber "<<lum(amber)<<", dark "<<lum(dark)<<"; LINKED "<<lum(unlinked)<<" -> "<<lum(linked)<<'\n';
}
void sideModelDisplay()
{
    std::cout<<"SECTION side_model_display\n";
    SideRig rig;
    auto& rd=*rig.ai.readout;
    juce::StringArray wrong;
    for(auto [key,want]:std::initializer_list<std::pair<const char*,const char*>>{{"ai","MODEL"},{"offline","OFFLINE"},{"account0","NO MODEL"},
        {"account1","CHOOSE MODEL"},{"account2","STUDIO FAST"}})
    {
        rig.show(key);
        if(rd.text.shown!=want||std::abs(rd.text.ink().getCentreX()-rd.centreX())>=0.01f)wrong.add(juce::String(key)+" '"+rd.text.shown+"'");
    }
    CHECK(wrong.isEmpty());
    if(!wrong.isEmpty())std::cout<<"model display: "<<wrong.joinIntoString(", ")<<'\n';
    rd.show("STUDIO EXTRA LARGE EXPERIMENTAL");
    const auto& t=rd.text;
    const auto minH=(float)(double)rig.model.screens["min_hscale"];
    CHECK(t.shown==juce::String("STUDIO EXTRA L")+juce::String::charToString((juce::juce_wchar)0x2026));
    CHECK(std::abs(t.hscale-minH)<1e-4f&&t.ink().getX()>=7.99f&&t.ink().getRight()<=(float)rd.getWidth()-7.99f);
    auto* h=rd.getAccessibilityHandler();
    CHECK(h!=nullptr&&h->getDescription()=="STUDIO EXTRA LARGE EXPERIMENTAL");
}
void sidePages()
{
    std::cout<<"SECTION side_pages\n";
    SideRig rig;
    juce::StringArray wrong;
    const std::array<std::array<const char*,4>,7> states{{{"ai","proposal","title","PROPOSAL READY"},{"create","create","title","CREATE SOUND"},
        {"offline","alert","heading","AI OFFLINE"},{"account0","signed_out","heading","SIGNED OUT"},{"account1","connected","heading","CONNECTED"},
        {"account2","linked","heading","LINKED: NIGHT SESSION"},{"library","library","list.0.0","EMBER REED"}}};
    for(const auto& [key,page,slot,text]:states)
    {
        rig.show(key);
        const auto& s=juce::String(key)=="library"?*rig.library.screen:*rig.ai.screen;
        if(s.page!=page||shownText(s,slot)!=text)wrong.add(juce::String(key)+" -> "+s.page+" '"+shownText(s,slot)+"'");
    }
    CHECK(wrong.isEmpty());
    if(!wrong.isEmpty())std::cout<<"pages: "<<wrong.joinIntoString(", ")<<'\n';
}
void sideTextPlacement()
{
    std::cout<<"SECTION side_text_placement\n";
    SideRig rig;
    std::map<juce::String,float> offGrid;
    if(auto* og=rig.model.screens["pages"]["library"]["list"]["off_grid"].getArray())
        for(const auto& o:*og)offGrid[o["text"].toString()]=(float)(double)o["dx"];
    // The drawings' texts the studio no longer draws: REFINE RACK's target, the LIBRARY's tabs and its three rows, which
    // five rows now replace. No drawing shows CREATE SOUND's page.
    const std::map<juce::String,juce::StringArray> gone{{"ai",{"TARGET","EMBER REED","SYNTH","1"}},
        {"library",{"EXAMPLES","MY DESIGNS","GENERIC","EMBER REED","HALLORAN","SYNTH","4U","GLASS CURRENT","KUROSHIO","2U","THREAD PRESS","MARLOW & FINCH","FX"}}};
    const std::pair<const char*,int> states[]{{"ai",18},{"offline",4},{"account0",5},{"account1",8},{"account2",19},{"library",5}};
    for(const auto& [key,count]:states)
    {
        rig.show(key);
        const auto& s=juce::String(key)=="library"?*rig.library.screen:*rig.ai.screen;
        auto worstX=0.0f,worstB=0.0f;
        juce::StringArray missing;
        int matched=0;
        auto runs=drawnRuns(rig.model,key);
        // Alpha access copy replaces these drawing labels at the same measured text anchors.
        if(juce::String(key)=="account0")
            for(auto& run:runs)
            {
                if(run.text=="Sign in to create sounds and racks.")run.text="Sound generation is experimental.";
                else if(run.text=="Your browser opens to sign in.")run.text="Account access and limits may vary.";
            }
        if(const auto g=gone.find(key);g!=gone.end())std::erase_if(runs,[&](const DrawnRun& r){return g->second.contains(r.text);});
        for(const auto& r:runs)
        {
            const studio::ScreenText* best=nullptr;
            juce::String bestKey;
            auto bestD=1.0e9f;
            for(const auto& [k,t]:s.texts)
            {
                if(t.path.isEmpty()||!t.shown.contains(r.text))continue;
                const auto dd=std::abs(t.ink().getBottom()-r.ink.getBottom())+0.01f*std::abs(t.ink().getX()-r.ink.getX());
                if(dd<bestD){bestD=dd;best=&t;bestKey=k;}
            }
            if(best==nullptr||std::abs(best->ink().getBottom()-r.ink.getBottom())>4.0f){missing.add(r.text);continue;}
            ++matched;
            const auto ink=best->ink();
            worstB=std::max(worstB,std::abs(ink.getBottom()-r.ink.getBottom()));
            if(juce::String(key)=="library"&&offGrid.count(r.text)&&bestKey.startsWith("list."))
                continue;
            if(s.centred.count(bestKey)&&best->shown==r.text)worstX=std::max(worstX,std::abs(ink.getCentreX()-r.ink.getCentreX()));
            else if(best->shown.startsWith(r.text))worstX=std::max(worstX,std::abs(ink.getX()-r.ink.getX()));
        }
        const auto passed=matched==count&&(int)runs.size()==count&&worstX<=1.0f&&worstB<=2.0f;
        CHECK(passed);
        std::cout<<key<<" "<<matched<<"/"<<runs.size()<<" left "<<worstX<<" bottom "<<worstB
            <<(missing.isEmpty()?juce::String():" not found: "+missing.joinIntoString(", "))<<'\n';
    }
}
void sideRoles()
{
    std::cout<<"SECTION side_roles\n";
    SideRig rig;
    rig.show("ai");
    juce::StringArray wrong;
    for(auto& b:rig.ai.buttons)
    {
        auto* h=b->getAccessibilityHandler();
        const auto role=b->isMode()?juce::AccessibilityRole::toggleButton:juce::AccessibilityRole::button;
        if(h==nullptr||h->getRole()!=role||h->getTitle()!=b->element.label
            ||(b->isMode()&&!h->getCurrentState().isChecked()))
            wrong.add(b->element.id);
    }
    for(auto& t:rig.ai.targets)
    {
        auto* h=t->getAccessibilityHandler();
        if(h==nullptr||h->getRole()!=juce::AccessibilityRole::button||h->getTitle()!=t->element.label||h->getDescription()!=t->state())
            wrong.add(t->element.id);
    }
    if(rig.ai.get<studio::Target>("ai.account")->state()!="connected")wrong.add("ai.account state");
    auto* rh=rig.ai.readout->getAccessibilityHandler();
    if(rh==nullptr||rh->getRole()!=juce::AccessibilityRole::button||rh->getDescription()!="MODEL")wrong.add("ai.model");
    auto* sh=rig.ai.screen->getAccessibilityHandler();
    const auto value=sh!=nullptr&&sh->getValueInterface()!=nullptr?sh->getValueInterface()->getCurrentValueAsString():juce::String();
    if(sh==nullptr||sh->getRole()!=juce::AccessibilityRole::staticText||!value.contains("PROPOSAL READY")||!value.contains("BLOOM"))
        wrong.add("ai.screen");
    rig.show("library");
    for(auto& b:rig.library.buttons)
    {
        auto* h=b->getAccessibilityHandler();
        if(h==nullptr||h->getRole()!=juce::AccessibilityRole::button||h->getTitle()!=b->element.label)wrong.add(b->element.id);
    }
    auto* lh=rig.library.screen->getAccessibilityHandler();
    if(lh==nullptr||lh->getValueInterface()==nullptr||!lh->getValueInterface()->getCurrentValueAsString().contains("GLASS CURRENT"))
        wrong.add("library.screen");
    CHECK(wrong.isEmpty()&&rig.ai.getTitle()=="AI"&&rig.library.getTitle()=="Library");
    if(!wrong.isEmpty())std::cout<<"wrong roles: "<<wrong.joinIntoString(", ")<<'\n';
}
void sideTodayIds()
{
    std::cout<<"SECTION side_today_ids\n";
    SideRig rig;
    const std::pair<const char*,const char*> buttons[]{{"ai.account","ai.account"},{"ai.model","ai.model"},
        {"ai.generate","ai.generate"},{"ai.refine","ai.refine"},
        {"ai.preview","ai.audition"},{"ai.stop","ai.stop"},{"ai.apply","ai.apply"},{"ai.discard","ai.discard"},{"ai.linked","ai.linked"},
        {"library.apply","library.apply"},
        {"library.cancel","library.cancel"}};
    for(const auto& [id,element]:buttons)
    {
        auto* b=dynamic_cast<juce::Button*>(find(rig,id));
        CHECK(b!=nullptr&&b->getProperties()["element"].toString()==element);
    }
    CHECK(find(rig,"ai.audition")==nullptr);
    CHECK(dynamic_cast<studio::Readout*>(find(rig,"ai.model"))!=nullptr);
    CHECK(dynamic_cast<studio::Target*>(find(rig,"ai.account"))!=nullptr&&dynamic_cast<studio::Target*>(find(rig,"ai.linked"))!=nullptr);
    CHECK(dynamic_cast<studio::Screen*>(find(rig,"ai.screen"))!=nullptr&&dynamic_cast<studio::Screen*>(find(rig,"library.screen"))!=nullptr);
}
void rightClickAt(juce::Component& c,Pt viewPoint)
{
    const auto at=local(c,viewPoint);
    c.mouseDown(event(c,at,juce::ModifierKeys::rightButtonModifier,1,false));
    c.mouseUp(event(c,at,{},1,false));
}
// A click (or right-click) on a hit of the page as it is now.
bool pressHit(studio::Screen& s,const studio::Screen::Hit* h,bool back=false)
{
    if(h==nullptr)return false;
    const auto at=h->r.getCentre()+s.getPosition().toFloat();
    if(back)rightClickAt(s,at);else clickAt(s,at);
    return true;
}
using Reported=std::pair<juce::String,int>;
using Entered=std::pair<juce::String,juce::String>;
void sideOptions()
{
    std::cout<<"SECTION side_options\n";
    SideRig rig;
    auto& scr=*rig.ai.screen;
    rig.show("create");
    CHECK(pressHit(scr,scr.hitFor("ai.scope")));
    CHECK(!rig.reported.empty()&&rig.reported.back()==Reported("ai.scope",1));
    CHECK(pressHit(scr,scr.hitFor("ai.scope"),true));
    CHECK(rig.reported.back()==Reported("ai.scope",2));
    // The four options fit the glass, so the wheel leaves the page where it is.
    for(int i=0;i<3;++i)scr.mouseWheelMove(event(scr,{20.0f,150.0f},{},0,false),wheel(-1.0f));
    CHECK(scr.scrollOf("create")==0&&shownText(scr,"option.3.label")=="BANK REUSE"&&scr.hitFor("ai.allowReuse")!=nullptr);
    const auto reports=rig.reported.size();
    const auto* notes=scr.hitFor("ai.requirePattern");
    CHECK(notes==nullptr&&shownText(scr,"option.1.label")=="EDITABLE NOTES");
    clickAt(scr,Pt(60.0f,scr.texts.at("option.1.label").ink().getCentreY())+scr.getPosition().toFloat());
    CHECK(rig.reported.size()==reports);
}
void sidePromptAndContext()
{
    std::cout<<"SECTION side_prompt_and_context\n";
    SideRig rig;
    auto& scr=*rig.ai.screen;
    auto d=drawnSide(rig.model,"ai");
    rig.show(d);
    CHECK(pressHit(scr,scr.hitFor("ai.prompt")));
    CHECK(scr.entry.isVisible()&&scr.entry.isMultiLine()&&scr.entry.getText()==d.screen.prompt&&scr.entry.getComponentID()=="ai.prompt");
    const juce::String typed="A warmer tape echo.\nLess hiss.\nKeep the drive.";
    scr.entry.setText(typed,false);
    scr.entry.onReturnKey();
    CHECK(scr.entry.isVisible()&&rig.entered.empty());
    scr.entry.onFocusLost();
    CHECK(!scr.entry.isVisible()&&rig.entered.size()==1&&rig.entered.back()==Entered("ai.prompt",typed));
    d.screen.prompt=typed;
    rig.show(d);
    CHECK(shownText(scr,"prompt.0")=="Less hiss."&&shownText(scr,"prompt.1")=="Keep the drive.");
    CHECK(pressHit(scr,scr.hitFor("ai.key")));
    CHECK(scr.entry.isVisible()&&!scr.entry.isMultiLine()&&scr.entry.getText()=="D"&&scr.entry.getComponentID()=="ai.key");
    scr.entry.setText(" F# ",false);
    scr.entry.onReturnKey();
    CHECK(rig.entered.size()==2&&rig.entered.back()==Entered("ai.key","F#"));
    d.screen.context[0].second="F#";
    rig.show(d);
    CHECK(shownText(scr,"context.KEY")=="KEY F#");
    CHECK(pressHit(scr,scr.hitFor("ai.tempo")));
    scr.entry.setText("96",false);
    scr.entry.onEscapeKey();
    CHECK(!scr.entry.isVisible()&&rig.entered.size()==2);
    d.screen.context={{"KEY",""},{"SCALE","MINOR"},{"ROLE","LEAD"},{"BPM",""}};
    rig.show(d);
    CHECK(shownText(scr,"context.KEY")=="KEY"&&shownText(scr,"context.BPM")=="HOST BPM");
}
void sideRows()
{
    std::cout<<"SECTION side_rows\n";
    SideRig rig;
    auto& scr=*rig.ai.screen;
    auto& lib=*rig.library.screen;
    const auto last=[&]{return rig.reported.empty()?Reported():rig.reported.back();};
    rig.show("account1");
    CHECK(pressHit(scr,scr.hitFor("ai.model",2))&&last()==Reported("ai.model",2));
    rig.show("account2");
    juce::StringArray wrong;
    for(int i=0;i<3;++i)
        if(!pressHit(scr,scr.hitFor("group.peer",i))||last()!=Reported("group.peer",i))wrong.add("peer "+juce::String(i));
    CHECK(wrong.isEmpty());
    CHECK(pressHit(scr,scr.hitFor("group.name")));
    CHECK(scr.entry.isVisible()&&scr.entry.getComponentID()=="group.name"&&scr.entry.getBounds().toFloat().contains(scr.texts.at("heading").ink().getCentre()));
    scr.entry.setText("Evening",false);
    scr.entry.onReturnKey();
    CHECK(!rig.entered.empty()&&rig.entered.back()==Entered("group.name","Evening"));
    CHECK(pressHit(scr,scr.hitFor("ai.page.join"))&&last()==Reported("ai.page.join",-1));
    CHECK(pressHit(scr,scr.hitFor("group.refresh"))&&last()==Reported("group.refresh",-1));
    CHECK(pressHit(scr,scr.hitFor("group.leave"))&&last()==Reported("group.leave",-1));
    auto join=drawnSide(rig.model,"account2");
    join.screen.page="join";join.screen.heading="JOIN A GROUP";join.screen.body={"Choose a discovered group."};
    join.screen.list={"Night Session 1a2b3c4d","Morning Takes 5e6f7a8b"};
    rig.show(join);
    CHECK(pressHit(scr,scr.hitFor("group.join",1))&&last()==Reported("group.join",1)&&shownText(scr,"row.1")=="MORNING TAKES 5E6F7A8B");
    auto pending=drawnSide(rig.model,"account2");
    pending.screen.heading="LINK PENDING: NIGHT SESSION";pending.screen.joinLabel="RESUME";pending.screen.leaveEnabled=false;
    rig.show(pending);
    const auto reports=rig.reported.size();
    const auto leave=rig.model.screens["pages"]["linked"]["buttons"][3]["rect"];
    const Rect leaveRect((float)(double)leave[0],(float)(double)leave[1],(float)(double)leave[2],(float)(double)leave[3]);
    clickAt(scr,leaveRect.getCentre()+scr.getPosition().toFloat());
    CHECK(shownText(scr,"button.1")=="RESUME"&&shownText(scr,"button.3")=="LEAVE"&&scr.hitFor("group.leave")==nullptr&&rig.reported.size()==reports);
    CHECK(pressHit(scr,scr.hitFor("group.resume"))&&last()==Reported("group.resume",-1));
    rig.show("library");
    CHECK(pressHit(lib,lib.hitFor("library.example",2))&&last()==Reported("library.example",2));
    CHECK(pressHit(lib,lib.hitFor("library.placement"))&&last()==Reported("library.placement",1));
    CHECK(pressHit(lib,lib.hitFor("library.placement"),true)&&last()==Reported("library.placement",-1));
    CHECK(pressHit(lib,lib.hitFor("library.allocation")));
    CHECK(lib.entry.isVisible()&&lib.entry.getComponentID()=="library.allocation");
    lib.entry.setText("0:1",false);
    lib.entry.onReturnKey();
    CHECK(rig.entered.back()==Entered("library.allocation","0:1"));
    CHECK(pressHit(lib,lib.hitFor("library.reuse"))&&last()==Reported("library.reuse",-1));
}
void sideActionButton()
{
    std::cout<<"SECTION side_action_button\n";
    SideRig rig;
    auto& scr=*rig.ai.screen;
    const auto inkOf=[&](const juce::String& key){return scr.texts.count(key)?scr.texts.at(key).ink():Rect();};
    const auto rectOf=[&](const juce::String& command){const auto* h=scr.hitFor(command);return h!=nullptr?h->r:Rect();};
    auto pending=drawnSide(rig.model,"create");
    pending.screen.page="proposal";pending.screen.title="GENERATING";pending.screen.action="CANCEL";pending.screen.status="WAITING FOR THE COMPANION";
    rig.show(pending);
    const auto cancelRect=rectOf("ai.cancel");
    const auto cancelPad=(cancelRect.getWidth()-inkOf("cancel").getWidth())/2.0f;
    CHECK(shownText(scr,"cancel")=="CANCEL"&&shownText(scr,"title")=="GENERATING"&&inkOf("title").getRight()+8.0f<=cancelRect.getX());
    CHECK(juce::String(cancelPad,1)=="18.0");
    CHECK(pressHit(scr,scr.hitFor("ai.cancel"))&&rig.reported.back()==Reported("ai.cancel",-1));
    rig.show("ai");
    CHECK(rectOf("ai.cancel").isEmpty()&&scr.texts.count("cancel")==0);
    std::cout<<"CANCEL padding: "<<cancelPad<<" px\n";
}
// The status line is cut to the glass, so its tooltip is the whole sentence as the unit wrote it, such as a
// generation failure with its log folder. An item's own tip still wins over its rect, and leaving clears it.
void sideStatusTooltip()
{
    std::cout<<"SECTION side_status_tooltip\n";
    SideRig rig;
    auto& scr=*rig.ai.screen;
    auto d=drawnSide(rig.model,"create");
    d.screen.status="Generation failed: provider_failure; network_failure; HTTP 503; server_error; request 7. Logs: C:\\Users\\Example\\AppData\\Local\\AI Rack Synth\\Logs";
    rig.show(d);
    CHECK(scr.texts.count("status")==1);
    if(scr.texts.count("status")!=1)return;
    const auto line=scr.texts.at("status");
    CHECK(line.text==d.screen.status.toUpperCase()&&line.shown!=line.text&&scr.hitAt(line.ink().getCentre())==nullptr);
    scr.mouseMove(event(scr,line.ink().getCentre(),{},0,false));
    CHECK(scr.getTooltip()==d.screen.status);
    const auto* scope=scr.hitFor("ai.scope");
    CHECK(scope!=nullptr);
    if(scope!=nullptr){scr.mouseMove(event(scr,scope->r.getCentre(),{},0,false));CHECK(scr.getTooltip()==scope->tip);}
    scr.mouseMove(event(scr,line.ink().getCentre(),{},0,false));
    scr.mouseExit(event(scr,line.ink().getCentre(),{},0,false));
    CHECK(scr.getTooltip().isEmpty());
    d.screen.status="READY TO GENERATE";
    rig.show(d);
    scr.mouseMove(event(scr,scr.texts.at("status").ink().getCentre(),{},0,false));
    CHECK(scr.getTooltip()=="READY TO GENERATE");
}
// The editor owns the one window that shows its controls' tooltips, the screens' and the knobs' alike.
void editorShowsTooltips()
{
    std::cout<<"SECTION editor_shows_tooltips\n";
    WindowRig w;
    int windows=0;
    for(auto* c:w.editor->getChildren())windows+=dynamic_cast<juce::TooltipWindow*>(c)!=nullptr?1:0;
    CHECK(windows==1);
}
void screenKeyboard()
{
    std::cout<<"SECTION screen_keyboard\n";
    SideRig rig;
    auto& scr=*rig.ai.screen;
    const auto key=[&](int code){return scr.keyPressed(juce::KeyPress(code));};
    const auto at=[&]{return juce::isPositiveAndBelow(scr.cursor(),(int)scr.hits.size())?&scr.hits[(size_t)scr.cursor()]:nullptr;};
    const auto description=[&]{auto* h=scr.getAccessibilityHandler();return h!=nullptr?h->getDescription():juce::String();};
    auto d=drawnSide(rig.model,"create");
    d.screen.options["ai.scope"]=1;d.screen.dim.clear();
    rig.show(d);
    auto& md=*rig.ai.readout;
    md.grabKeyboardFocus();
    CHECK(wait("the model display has the focus",[&]{return md.hasKeyboardFocus(false);}));
    md.moveKeyboardFocusToSibling(true);
    CHECK(wait("the screen has the focus",[&]{return scr.hasKeyboardFocus(false);}));
    CHECK(at()!=nullptr&&at()->entry=="ai.prompt");
    if(at()!=nullptr)
    {
        const auto r=at()->r;
        const auto ring=snap(scr).getPixelAt(juce::roundToInt(r.getCentreX()),(int)r.getY());
        CHECK(std::abs(ring.getRed()-studio::accent.getRed())<=3&&std::abs(ring.getGreen()-studio::accent.getGreen())<=3
            &&std::abs(ring.getBlue()-studio::accent.getBlue())<=3);
    }
    CHECK(key(juce::KeyPress::downKey)&&at()!=nullptr&&at()->command=="ai.scope"&&description()=="SCOPE: PATTERN");
    CHECK(key(juce::KeyPress::rightKey)&&rig.reported.back()==Reported("ai.scope",2));
    CHECK(key(juce::KeyPress::leftKey)&&rig.reported.back()==Reported("ai.scope",0));
    key(juce::KeyPress::downKey);
    CHECK(at()!=nullptr&&at()->command=="ai.requirePattern"&&description()=="EDITABLE NOTES: OFF");
    key(juce::KeyPress::downKey);key(juce::KeyPress::downKey);
    CHECK(at()!=nullptr&&at()->command=="ai.allowReuse"&&scr.scrollOf("create")==0);
    key(juce::KeyPress::downKey);
    CHECK(at()!=nullptr&&at()->entry=="ai.key"&&scr.scrollOf("create")==0);
    CHECK(key(juce::KeyPress::rightKey)&&at()!=nullptr&&at()->entry=="ai.scale");
    CHECK(key(juce::KeyPress::leftKey)&&at()!=nullptr&&at()->entry=="ai.key");
    CHECK(key(juce::KeyPress::returnKey)&&scr.entry.isVisible()&&scr.entry.getComponentID()=="ai.key");
    scr.entry.setText("Q",false);
    scr.entry.onEscapeKey();
    CHECK(!scr.entry.isVisible()&&rig.entered.empty()&&scr.hasKeyboardFocus(false));
    CHECK(key(juce::KeyPress::spaceKey)&&scr.entry.isVisible()&&scr.entry.getComponentID()=="ai.key");
    scr.entry.setText("E",false);
    scr.entry.onReturnKey();
    CHECK(!rig.entered.empty()&&rig.entered.back()==Entered("ai.key","E")&&scr.hasKeyboardFocus(false));
    CHECK(!key(juce::KeyPress::escapeKey)&&rig.escapes==0);
    auto proposal=drawnSide(rig.model,"ai");
    proposal.screen.changes.push_back({"DRIVE","0.40","0.55"});
    rig.show(proposal);
    CHECK(at()!=nullptr&&at()->entry=="ai.prompt");
    for(int i=0;i<7;++i)key(juce::KeyPress::downKey);
    CHECK(at()!=nullptr&&at()->row==2&&shownText(scr,"change.2.name")=="AIR");
    key(juce::KeyPress::downKey);
    CHECK(scr.scrollOf("proposal")==1&&shownText(scr,"change.2.name")=="DRIVE"&&description()=="DRIVE: 0.40 TO 0.55");
    rig.show("account1");
    CHECK(at()!=nullptr&&at()->command=="ai.model"&&at()->row==0&&shownText(scr,"row.0").isNotEmpty());
    key(juce::KeyPress::downKey);key(juce::KeyPress::downKey);
    CHECK(at()!=nullptr&&at()->row==2&&description()=="STUDIO MINI");
    const auto beside=juce::roundToInt(scr.hits[2].r.getRight()-6.0f);
    const auto bar=snap(scr).getPixelAt(beside,juce::roundToInt(scr.hits[2].r.getCentreY()));
    const auto plain=snap(scr).getPixelAt(beside,juce::roundToInt(scr.hits[0].r.getCentreY()));
    CHECK(bar.getBrightness()>plain.getBrightness()+0.3f);
    CHECK(key(juce::KeyPress::returnKey)&&rig.reported.back()==Reported("ai.model",2));
    CHECK(key(juce::KeyPress::escapeKey)&&rig.escapes==1);
    CHECK(!key(juce::KeyPress::tabKey));
    scr.moveKeyboardFocusToSibling(true);
    auto* next=juce::Component::getCurrentlyFocusedComponent();
    CHECK(next!=nullptr&&next->getProperties()["element"].toString()=="ai.generate");
}
void screenEntryTeardown()
{
    std::cout<<"SECTION screen_entry_teardown\n";
    {
        SideRig rig;
        auto& scr=*rig.ai.screen;
        auto d=drawnSide(rig.model,"create");
        rig.show(d);
        pressHit(scr,scr.hitFor("ai.key"));
        scr.entry.setText("G",false);
        d.screen.status="ANOTHER STATUS";
        rig.show(d);
        CHECK(scr.entry.isVisible()&&scr.entry.getText()=="G");
        rig.show("account1");
        CHECK(!scr.entry.isVisible()&&rig.entered.empty());
    }
    int late=0;
    {
        auto rig=std::make_unique<SideRig>();
        rig->show("create");
        auto& scr=*rig->ai.screen;
        scr.onEntry=[&late](const juce::String&,const juce::String&){++late;};
        pressHit(scr,scr.hitFor("ai.prompt"));
        CHECK(wait("the prompt entry has the focus",[&]{return scr.entry.hasKeyboardFocus(false);}));
        scr.entry.setText("Late words",false);
        rig.reset();
        pump();
    }
    CHECK(late==0);
}
// The LIBRARY on the desktop over a prepared processor that never launches the companion. The host records the sound's revision when each frame it gives is taken and released, and says the
// editor is closing when told to.
struct LibraryRig
{
    InstrumentProcessor p{WindowRig::offline(),airack::testing::isolatedRackRuntime()};
    FakeServices services;
    bool closing=false;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> frames;
    std::vector<juce::Uuid> placed;
    juce::StringArray events;
    std::unique_ptr<studio::LibraryUnit> unit;
    LibraryRig(){
        CHECK(blankInstance(p));p.prepareToPlay(48000,64);
        studio::LibraryUnit::Host host;
        host.frame=[this]()->std::shared_ptr<void>{
            struct Held{LibraryRig& rig;std::size_t index;Held(LibraryRig& r,std::size_t i):rig(r),index(i){}~Held(){rig.frames[index].second=rig.revision();}};
            frames.push_back({revision(),0});return std::make_shared<Held>(*this,frames.size()-1);
        };
        host.closing=[this]{return closing;};
        unit=std::make_unique<studio::LibraryUnit>(p,*studio::studioSkin(),services.make(),host);
        unit->onPlaced=[this](juce::Uuid id,const juce::String&){placed.push_back(id);events.add("placed");};
        unit->onDone=[this]{events.add("done");};unit->addToDesktop(0);unit->setVisible(true);
        CHECK(wait("local Gallery inventory",[&]{unit->refresh();return unit->matchingRows()==17;}));
    }
    ~LibraryRig(){unit.reset();p.releaseResources();}
    std::uint64_t revision(){SoundDocument d;std::uint64_t r=0,reset=0;CHECK(p.trySoundSnapshot(d,r,reset));return r;}
    void refresh(){unit->refresh();pump(1);}
    bool choose(const char* name){return chooseLibrary(*unit,name);}
    juce::String line(){auto* label=widget<juce::Label>(*unit,"library.diff");return label?label->getText():juce::String{};}
    juce::String status(){auto* label=widget<juce::Label>(*unit,"library.status");return label?label->getText():juce::String{};}
    juce::String difference(){auto* text=widget<juce::TextEditor>(*unit,"library.description");return text?text->getText().fromFirstOccurrenceOf("PLACEMENT REVIEW\n",false,false):juce::String{};}
    juce::ComboBox& place(){return *widget<juce::ComboBox>(*unit,"library.placement");}
    void press(const char* id){click(*unit,id);}
    std::size_t racks(){const auto d=document(p);return d.personalities?d.personalities->instances.size():0;}
    void addEffect(std::uint8_t chain,EffectKind kind){const auto d=document(p);std::uint8_t slot=0;while(slot<8&&d.layout.effects[slot].use==SlotUse::present)++slot;CHECK(p.applyRackEdit({RackEditKind::addEffect,slot,chain,(std::uint8_t)d.layout.effectCount[chain],kind},revision())==RackEditResult::applied);refresh();}
    void addDrive(std::uint8_t chain){addEffect(chain,EffectKind::drive);}
};
// The studio feedback spec's decision 1: racks are made outside the plugin, so the AI unit has no CREATE RACK or
// REFINE RACK and the LIBRARY no LOAD FILE, MY DESIGNS or GENERIC.
void racksPlacedNotMade()
{
    std::cout<<"SECTION racks_placed_not_made\n";
    SideRig rig;
    for(const auto* gone:{"ai.create_rack","ai.refine_rack","library.load_file","ai.create_sound","library.review"})CHECK(find(rig,gone)==nullptr);
    LibraryRig library;
    CHECK(find(*library.unit,"library.load_file")==nullptr&&find(*library.unit,"library.review")==nullptr);
    // Decision 2: the AI unit's actions and the LIBRARY's APPLY light the mode LED's sprite, sized to each lens.
    const auto& sprite=rig.model.asset("ai:mode.lit");
    for(const juce::String id:{"ai.generate","ai.refine","ai.audition","ai.apply","library.apply"})
    {
        const studio::SkinButton* button=nullptr;
        for(auto* v:{&rig.ai,&rig.library})for(const auto& b:v->buttons)if(b->element.id==id)button=b.get();
        CHECK(button!=nullptr);if(button==nullptr)continue;
        const auto scale=sprite.lensR>0?button->element.lensR/sprite.lensR:0.0f;
        const auto lit=studio::Rect((float)sprite.image.getWidth(),(float)sprite.image.getHeight()).withPosition(-sprite.anchor)*scale+button->element.led;
        CHECK(button->element.layer("lit")=="ai:mode.lit");CHECK(std::abs(button->litScale-scale)<1e-4f);CHECK(button->getBounds().toFloat().contains(lit));
    }
}
// Explicit Add places the selected exact recipe in one edit and reports bank need; capacity refusal preserves sound.
void libraryOnePress()
{
    std::cout<<"SECTION library_one_press\n";
    LibraryRig rig;CHECK(rig.choose("Copper Arc"));const auto before=document(rig.p);const auto edit=rig.revision();
    CHECK(rig.line().contains("NEEDS 1 SYNTH / 0 FX BANKS")&&find(*rig.unit,"library.review")==nullptr);
    rig.press("library.apply");CHECK(rig.racks()==1&&rig.revision()!=edit&&rig.events==juce::StringArray({"placed","done"}));
    CHECK(rig.p.undoSoundEdit());rig.refresh();CHECK(document(rig.p)==before&&rig.racks()==0);
    LibraryRig full;for(std::uint8_t slot=1;slot<4;++slot)CHECK(full.p.applyRackEdit({RackEditKind::addSynth,slot,masterChain,slot},full.revision())==RackEditResult::applied);
    full.refresh();CHECK(full.choose("Copper Arc"));const auto unchanged=document(full.p);full.press("library.apply");CHECK(full.status().contains("Capacity reached")&&document(full.p)==unchanged&&full.events.isEmpty());
}
// A retained FX recipe exposes its destination, explicit reuse and exact bank fields; Add is one rack transaction.
void libraryWorkflow()
{
    std::cout<<"SECTION library_workflow\n";LibraryRig rig;CHECK(rig.choose("Echo Thread"));CHECK(rig.place().getText().startsWith("SHARED MASTER"));
    const auto before=document(rig.p);rig.press("library.reuse");CHECK(widget<juce::Button>(*rig.unit,"library.reuse")->getToggleState());
    auto* banks=widget<juce::TextEditor>(*rig.unit,"library.allocation");banks->setText("0:4",true);pump();CHECK(banks->getText()=="0:4");
    rig.press("library.reuse");CHECK(!widget<juce::Button>(*rig.unit,"library.reuse")->getToggleState());banks->setText({},true);pump();
    CHECK(document(rig.p)==before);rig.press("library.apply");CHECK(rig.racks()==1&&rig.events==juce::StringArray({"placed","done"}));
    const auto placed=document(rig.p);CHECK(placed.personalities&&!placed.personalities->instances.empty());if(placed.personalities&&!placed.personalities->instances.empty())CHECK(placed.personalities->instances.back().design.name=="Echo Thread");
}
// The Gallery's retained AIR recipes use the panel's shared review/apply path with explicit localDesign.
// The panel's six detached examples remain covered by RackPanelTests; their indexes are not Gallery identities.
void libraryMatchesNativeReview()
{
    std::cout<<"SECTION library_matches_native_review\n";LibraryRig rig;rig.addDrive(masterChain);
    for(const auto* name:{"Copper Arc","Echo Thread"}){
        const auto definitions=airack::test::frozenLibraryRackDesigns();const auto at=std::find_if(definitions.begin(),definitions.end(),[&](const auto& d){return d.name==name;});CHECK(at!=definitions.end());if(at==definitions.end())continue;
        const auto before=document(rig.p);juce::String panelStatus,panelDifference;
        RackReviewRequest request;request.localDesign=*at;request.chain=masterChain;request.position=0;
        const auto reviewed=reviewRack(rig.p,request);panelDifference=reviewed.difference;
        const auto applied=applyRackReview(rig.p,reviewed);CHECK(applied.applied);panelStatus=applied.status;
        CHECK(rig.p.undoSoundEdit());rig.refresh();CHECK(document(rig.p)==before);CHECK(rig.choose(name));rig.place().setSelectedId(1,juce::sendNotificationSync);rig.refresh();
        CHECK(!panelDifference.isEmpty()&&rig.difference()==panelDifference);rig.press("library.apply");CHECK(rig.racks()==1&&rig.status()==panelStatus);CHECK(rig.p.undoSoundEdit());rig.refresh();CHECK(document(rig.p)==before);
    }
}
// A recall while the LIBRARY is open: PLACE AS follows the recalled sound, and nothing is said. CANCEL is today's Cancel
// draft and closes the LIBRARY; the editor's cancel() does not.
void libraryRecall()
{
    std::cout<<"SECTION library_recall\n";LibraryRig rig;const auto saved=state(document(rig.p));rig.addDrive(masterChain);CHECK(rig.choose("Echo Thread"));CHECK(rig.place().getText().contains("AFTER"));
    CHECK(restore(rig.p,saved));rig.refresh();CHECK(rig.place().getText()=="SHARED MASTER, FIRST");const auto before=document(rig.p);rig.unit->cancel();CHECK(document(rig.p)==before&&rig.events.isEmpty());rig.press("library.cancel");CHECK(rig.events==juce::StringArray({"done"})&&rig.racks()==0);
}
// APPLY runs inside the host's frame; while the editor closes it applies nothing.
void libraryFrames()
{
    std::cout<<"SECTION library_frames\n";LibraryRig rig;CHECK(rig.choose("Copper Arc"));const auto before=rig.revision();rig.closing=true;rig.press("library.apply");CHECK(rig.revision()==before&&rig.racks()==0&&rig.events.isEmpty());rig.closing=false;rig.frames.clear();rig.press("library.apply");CHECK(rig.frames.size()==1&&rig.racks()==1&&rig.events==juce::StringArray({"placed","done"}));if(rig.frames.size()==1)CHECK(rig.frames[0].first==before&&rig.frames[0].second!=before);
}
// PLACE AS (ruling 10): a synth design steps through the synth order, an FX design through Shared Master and each
// placed synth's insert chain, each from FIRST to after its last unit, named by the rack that owns the unit before or
// by its kind; each starts at the end of its order, and APPLY places the design there.
void libraryPlaces()
{
    std::cout<<"SECTION library_places\n";LibraryRig rig;CHECK(rig.choose("Copper Arc"));rig.press("library.apply");rig.refresh();rig.addDrive(masterChain);CHECK(rig.choose("Echo Thread"));
    CHECK(rig.place().getNumItems()==4&&rig.place().getItemText(0)=="SHARED MASTER, FIRST"&&rig.place().getItemText(1).contains("AFTER DRIVE")&&rig.place().getItemText(2).contains("SYNTH 1 INSERT")&&rig.place().getItemText(3).contains("SYNTH 2 INSERT"));
    rig.place().setSelectedId(4,juce::sendNotificationSync);rig.press("library.apply");const auto d=document(rig.p);CHECK(rig.racks()==2);if(d.personalities){const auto& placed=d.personalities->instances.back();CHECK(d.layout.effects[placed.bindings.front().slot].chain==1);}
}
// The studio support spec's names in the LIBRARY: PLACE AS names an unowned EQ bank on the shared master by its kind.
void libraryKindNames()
{
    std::cout<<"SECTION library_kind_names\n";LibraryRig rig;rig.addEffect(masterChain,EffectKind::eq);CHECK(rig.choose("Echo Thread"));CHECK(rig.place().getText()=="SHARED MASTER, AFTER EQ");
}
// The Gallery exposes seventeen retained AIR recipes. Detached maker examples need packages; the frozen exporter stays intact.
void libraryFactoryDesigns()
{
    std::cout<<"SECTION library_factory_designs\n";LibraryRig rig;CHECK(rig.unit->matchingRows()==17);auto* search=widget<juce::TextEditor>(*rig.unit,"library.search");search->setText("Ember Reed",true);pump();CHECK(rig.unit->matchingRows()==0);search->setText({},true);pump();CHECK(rig.unit->matchingRows()==17);CHECK(rig.choose("Contour Loom"));rig.press("library.apply");CHECK(rig.racks()==1&&document(rig.p).personalities->instances.back().design.name=="Contour Loom");
    const auto library=airack::test::frozenLibraryRackDesigns();CHECK(library.size()==28&&airack::test::frozenExampleRackDesigns().size()==6&&airack::test::frozenFactoryRackDesigns().size()==21);
}
// Decision 13, the LIBRARY's side: its placement is the chosen design's preview at PLACE AS, lettered PREVIEW: and PLACE
// AS's words, at the design's own values, for each example; none while the LIBRARY is hidden.
void libraryPlacement()
{
    std::cout<<"SECTION library_placement\n";LibraryRig rig;const auto before=document(rig.p);CHECK(rig.choose("Copper Arc")&&!rig.unit->placement());rig.place().setSelectedId(1,juce::sendNotificationSync);CHECK(rig.place().getText()=="SYNTH 1, FIRST");CHECK(rig.choose("Echo Thread")&&!rig.unit->placement());CHECK(rig.line().contains("1 FX BANKS"));CHECK(document(rig.p)==before);rig.unit->setVisible(false);CHECK(!rig.unit->placement()&&rig.unit->ownedImageBytes()==0);
}
// The rack stack as the editor holds it (the rack stage spec): a drawer, the house unit entering the drawer's frame as
// the editor's does, the empty space and the stack, in the rack area's 1547 x 754 on the desktop.
struct StackRig
{
    InstrumentProcessor p{WindowRig::offline(), airack::testing::isolatedRackRuntime()};
    // Restored before the drawer exists: the drawer reports a restore it sees as a recall. A null start keeps the new
    // instance's own.
    const bool restored;
    const studio::Model& model=*studio::studioSkin();
    RackPanelComponent drawer{p,model};
    std::unique_ptr<studio::HouseUnit> house;
    studio::EmptyRackSpace space{model};
    juce::Component holder;
    std::unique_ptr<RackStack> stack;
    juce::StringArray reported;
    std::vector<juce::Uuid> selections;
    explicit StackRig(bool(*start)(InstrumentProcessor&)=blankInstance,studio::house::Host unitHost={}):restored(start==nullptr||start(p))
    {
        CHECK(restored);
        studio::house::Host host;
        host.frame=[this]{return drawer.editorCallbackFrame();};
        host.closing=[]{return false;};
        host.report=[this](const juce::String& s){reported.add(s);};
        house=std::make_unique<studio::HouseUnit>(p,model,std::move(host));
        stack=std::make_unique<RackStack>(p,model,*house,space,drawer,std::move(unitHost));
        stack->onReport=[this](const juce::String& s){reported.add(s);};
        stack->onEdited=[this]{refresh();};
        stack->onSelectionChanged=[this](juce::Uuid id){selections.push_back(id);};
        holder.setSize(1547,754);stack->setBounds(holder.getLocalBounds());holder.addAndMakeVisible(*stack);
        // Native peer hits require a point on a display; pressThrough raises the window for the gesture.
        holder.setTopLeftPosition(juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea.getPosition());
        // Keep the native input host raised for its lifetime, including focus-driven relayouts.
        holder.setAlwaysOnTop(true);
        holder.addToDesktop(0);holder.setVisible(true);refresh();pump();
    }
    ~StackRig() { stack.reset();house.reset();pump(); }
    void refresh() { const auto d=document(p);house->refresh(d);stack->refresh(d); }
    RackStackUnit* unit(int n) { return dynamic_cast<RackStackUnit*>(find(stack->body(),"rack.unit."+juce::String(n))); }
    juce::Component* divider() { return find(stack->body(),"rack.divider"); }
    // The drawer's status line. A rig that has only set up its sound shows none.
    juce::String status() { auto* line=widget<juce::Label>(drawer,"rack.status");return line!=nullptr?line->getText():juce::String("no status line"); }
    // A component's rectangle in the body.
    Rect at(juce::Component& c) { return stack->body().getLocalArea(&c,c.getLocalBounds().toFloat()); }
    // The open drawer under a rack: the all-controls panel on the rack's bottom, the form on the panel's.
    bool drawerUnder(juce::Component& rack)
    {
        const auto panel=at(stack->controls());
        return drawer.isShowing()&&std::abs(panel.getY()-at(rack).getBottom())<=1.0f&&std::abs(at(drawer).getY()-panel.getBottom())<=1.0f;
    }
    juce::Image body() { return snap(stack->body()); }
    std::array<std::uint64_t,4> revisions()
    {
        SoundDocument s;ProjectDocument d;std::uint64_t se=0,sr=0,pe=0,pr=0;
        CHECK(p.trySoundSnapshot(s,se,sr)&&p.tryProjectSnapshot(d,pe,pr));
        return {se,sr,pe,pr};
    }
    // The placed racks' IDs in placement order.
    std::vector<juce::Uuid> ids()
    {
        std::vector<juce::Uuid> all;const auto d=document(p);
        if(d.personalities!=nullptr)for(const auto& i:d.personalities->instances)all.push_back(i.id);
        return all;
    }
    // Whether c lies wholly in the stack's view.
    bool inView(juce::Component& c)
    {
        const auto a=at(c);const auto top=(float)stack->viewport().getViewPositionY();
        return a.getY()>=top-0.5f&&a.getBottom()<=top+(float)stack->viewport().getHeight()+0.5f;
    }
};
// A press and a release delivered through the window's peer, as the system delivers them: to the component under the
// point, with its mouse listeners and the focus a click moves. The peer delivers them only where its window is the top
// one on the desktop, and a window that CI's runner opens can lie behind another, so the window stays on top for the press.
void pressThrough(juce::Component& c,Pt at)
{
    auto* peer=c.getPeer();
    CHECK(peer!=nullptr);
    if(peer==nullptr)return;
    using Source=juce::MouseInputSource;
    auto& window=peer->getComponent();const bool onTop=window.isAlwaysOnTop();
    window.setAlwaysOnTop(true);
    const auto point=window.getLocalPoint(&c,at);
    // Activate the input window while waiting for a real desktop hit, for up to the existing second.
    for(int n=0;n<20&&!peer->contains(point.roundToInt(),true);++n){window.toFront(true);pump(50);}
    CHECK(peer->contains(point.roundToInt(),true));
    if(!peer->contains(point.roundToInt(),true))
        std::cout<<"  press at "<<point.toString()<<" outside the window, on screen "<<window.getScreenBounds().toString()<<'\n';
    const auto t=juce::Time::currentTimeMillis();
    peer->handleMouseEvent(Source::InputSourceType::mouse,point,juce::ModifierKeys::leftButtonModifier,Source::defaultPressure,Source::defaultOrientation,t);
    peer->handleMouseEvent(Source::InputSourceType::mouse,point,juce::ModifierKeys(),Source::defaultPressure,Source::defaultOrientation,t+1);
    window.setAlwaysOnTop(onTop);
    pump();
}
// A wheel turn over c at `at`, from the desktop's main mouse source.
void spin(juce::Component& c,Pt at,float dy) { c.mouseWheelMove(event(c,at,{},0,false),wheel(dy));pump(); }
// A pointer held through the window's peer, as pressThrough sends its events: a press, moves and a release, each at a
// point in c's coordinates, and Escape. The window stays on top for the gesture. It is a pen, a source of its own: the
// desktop's real mouse, moving over the window between the events, would release the main mouse source's button.
struct Pointer
{
    juce::Component& c;
    juce::int64 t=juce::Time::currentTimeMillis();
    Pt last;
    void send(Pt at,bool down)
    {
        auto* peer=c.getPeer();
        CHECK(peer!=nullptr);
        if(peer==nullptr)return;
        using Source=juce::MouseInputSource;
        auto& window=peer->getComponent();window.setAlwaysOnTop(true);last=at;
        const auto point=window.getLocalPoint(&c,at).roundToInt();
        for(int n=0;n<20&&window.getLocalBounds().contains(point)&&!peer->contains(point,true);++n){window.toFront(false);pump(50);}
        // JUCE drops the event that creates the desktop's pen source, so the pen's first event is a hover that creates it.
        const auto& sources=juce::Desktop::getInstance().getMouseSources();
        if(std::none_of(sources.begin(),sources.end(),[](const Source& s){return s.getType()==Source::InputSourceType::pen;}))
            peer->handleMouseEvent(Source::InputSourceType::pen,window.getLocalPoint(&c,at),juce::ModifierKeys(),Source::defaultPressure,
                                   Source::defaultOrientation,t++);
        peer->handleMouseEvent(Source::InputSourceType::pen,window.getLocalPoint(&c,at),down?juce::ModifierKeys::leftButtonModifier:juce::ModifierKeys(),
                               Source::defaultPressure,Source::defaultOrientation,t++);
        pump(20);
    }
    void press(Pt at) { send(at,true); }
    void move(Pt at) { send(at,true); }
    void release(Pt at) { send(at,false);if(auto* peer=c.getPeer())peer->getComponent().setAlwaysOnTop(false); }
    void escape() { if(auto* peer=c.getPeer())peer->handleKeyPress(juce::KeyPress(juce::KeyPress::escapeKey));pump(); }
};
// The placed racks' IDs in the stack's order for the live sound.
std::vector<juce::Uuid> stackIds(InstrumentProcessor& p)
{
    std::vector<juce::Uuid> all;
    for(const auto& s:rackOrder(document(p)))all.push_back(s.id);
    return all;
}
// The parameter that the first control of the placed rack `n` drives.
std::optional<std::size_t> firstControl(const SoundDocument& d,std::size_t n)
{
    if(d.personalities==nullptr||d.personalities->instances.size()<=n)return {};
    const auto& i=d.personalities->instances[n];
    if(i.design.panel.controls.empty())return {};
    const auto& c=i.design.panel.controls[0];
    const auto u=std::find_if(i.design.units.begin(),i.design.units.end(),[&](const auto& v){return v.id==c.unit;});
    const auto b=std::find_if(i.bindings.begin(),i.bindings.end(),[&](auto v){return v.unit==c.unit;});
    if(u==i.design.units.end()||b==i.bindings.end())return {};
    return rackParameterIndex(u->module,b->slot,c.key);
}
// The studio feedback spec's decision 4: a new instance's stack holds no rack and no bare synth bank; the empty space
// lies under the house unit.
void stack_new_instance()
{
    std::cout<<"SECTION stack_new_instance\n";
    StackRig r{nullptr};
    const auto d=document(r.p);
    CHECK(d.layout.synthCount==0&&(d.personalities==nullptr||d.personalities->instances.empty()));
    CHECK(r.unit(0)==nullptr&&find(r.stack->body(),"rack.bank.synth.1")==nullptr
          &&std::abs(r.at(r.space).getY()-3.0f*r.stack->unitHeight())<=1.0f);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-new-instance.png"),r.body()));
}
// The stack's order and heights (the rack stage spec: The stack): the house unit 3U, each rack 2U in the sound's order
// (Ember Reed's faceplate 3U), the divider 1U while the shared master has an FX rack, then the space, at least 1U and the
// rest of the area.
void stackOrder()
{
    std::cout<<"SECTION stack_order\n";
    {
        StackRig r;
        const auto u=r.stack->unitHeight();
        CHECK(std::abs(r.stack->scale()-1547.0f/2228.0f)<1e-4f&&std::abs(u-136.86f)<0.01f);
        // Synth 1's generic rack 2U under the house unit, then the space, 1U.
        auto* bank=find(r.stack->body(),"rack.bank.synth.1");
        CHECK(r.unit(0)==nullptr&&bank!=nullptr&&r.divider()!=nullptr&&!r.divider()->isVisible());
        if(bank!=nullptr)
            CHECK(std::abs(r.at(*bank).getY()-3.0f*u)<=1.0f&&std::abs(r.at(r.space).getY()-5.0f*u)<=1.0f&&std::abs(r.at(r.space).getBottom()-6.0f*u)<=1.0f);
        drawnRacks(r.p);r.refresh();pump();
        CHECK(r.unit(0)!=nullptr&&r.unit(1)!=nullptr&&r.unit(2)!=nullptr&&r.unit(3)==nullptr&&r.divider()!=nullptr&&r.divider()->isVisible());
        if(r.unit(2)==nullptr||r.divider()==nullptr)return;
        // The house unit 3U, EMBER REED 3U, GLASS CURRENT 2U, the divider 1U, THREAD PRESS 2U, then the space 1U.
        const float tops[]{r.at(*r.house).getY(),r.at(*r.unit(0)).getY(),r.at(*r.unit(1)).getY(),r.at(*r.divider()).getY(),r.at(*r.unit(2)).getY(),r.at(r.space).getY()};
        const float want[]{0.0f,3.0f,6.0f,8.0f,9.0f,11.0f};
        bool stacked=true;for(int n=0;n<6;++n)stacked=stacked&&std::abs(tops[n]-want[n]*u)<=1.0f;
        CHECK(stacked&&std::abs((float)r.stack->body().getHeight()-12.0f*u)<=1.0f&&std::abs(r.at(r.space).getBottom()-12.0f*u)<=1.0f);
        CHECK(r.unit(0)->name()=="EMBER REED"&&r.unit(0)->place()=="SYNTH 1"&&r.unit(1)->name()=="GLASS CURRENT"&&r.unit(1)->place()=="SYNTH 2"
              &&r.unit(2)->name()=="THREAD PRESS"&&r.unit(2)->place()=="SHARED MASTER");
        const auto d=document(r.p);const auto& ember=d.personalities->instances[0];
        CHECK(r.unit(0)->id()==ember.id&&r.unit(0)->getTitle()=="Ember Reed"
              &&r.unit(0)->getDescription()==rackHeaderText(d,ember)+"\nHALLORAN ELECTRONIC MUSIC CO., ANALOG VOICE MODEL 74");
        std::cout<<"  tops:";for(const auto t:tops)std::cout<<' '<<t;std::cout<<"; body "<<r.stack->body().getHeight()<<'\n';
    }
    // Thread Press on Ember Reed's chain instead: under it, SYNTH 1 INSERT, and no divider.
    {
        StackRig r;
        placeRack(r.p,0,{},{{0,0},{1,0}},true);
        placeRack(r.p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}});
        placeRack(r.p,2,{RackRole::fx,0,1},{{0,3},{1,4}});
        r.refresh();pump();
        CHECK(r.unit(1)!=nullptr&&r.unit(1)->name()=="THREAD PRESS"&&r.unit(1)->place()=="SYNTH 1 INSERT"&&r.unit(2)!=nullptr&&r.unit(2)->name()=="GLASS CURRENT"
              &&r.divider()!=nullptr&&!r.divider()->isVisible());
        if(r.unit(2)!=nullptr)CHECK(std::abs(r.at(*r.unit(2)).getY()-8.0f*r.stack->unitHeight())<=1.0f);
    }
}
// The rails (the rack stage spec: The rails): both number every unit from 1 at the stack's top through the space's whole
// units, centred in each; the selection light runs beside the selected rack, and elsewhere the rails are the fold
// rail's column.
void stackRails()
{
    std::cout<<"SECTION stack_rails\n";
    StackRig r;
    const auto u=r.stack->unitHeight(),k=r.stack->scale();
    const auto numbered=[&](int count)
    {
        const auto got=r.stack->railNumbers();bool ok=(int)got.size()==count;
        for(int n=0;ok&&n<count;++n)ok=got[(std::size_t)n].first==n+1&&std::abs(got[(std::size_t)n].second-((float)n+0.5f)*u)<=0.5f;
        return ok;
    };
    // The house unit 3U, synth 1's generic rack 2U, then the space's 1U.
    CHECK(numbered(6));
    drawnRacks(r.p);r.refresh();pump();
    // 3 + 3 + 2 + 1 + 2 units, then the space's 1U.
    CHECK(numbered(12)&&r.unit(0)!=nullptr&&r.unit(1)!=nullptr);
    if(r.unit(0)==nullptr||r.unit(1)==nullptr)return;
    const auto img=r.body();
    const float left=(float)studio::HouseUnit::railW*k,right=((float)studio::HouseUnit::railW+r.model.house.panelWidth)*k;
    bool inked=true;
    for(int n=1;n<=12;++n)
    {
        const auto y=((float)n-0.5f)*u;
        inked=inked&&peakLuma(img,Rect(left*0.5f-8.0f,y-8.0f,16.0f,16.0f))>100.0&&peakLuma(img,Rect((right+1547.0f)*0.5f-8.0f,y-8.0f,16.0f,16.0f))>100.0;
    }
    CHECK(inked);
    // Beside Ember Reed, selected as the first rack, the light on both rails' inner edges; beside Glass Current, the column.
    const auto at=[&](float x,float y){return img.getPixelAt((int)x,(int)y);};
    const auto mid0=r.at(*r.unit(0)).getCentreY(),mid1=r.at(*r.unit(1)).getCentreY();
    const auto lightL=at(left-1.5f,mid0),lightR=at(right+1.5f,mid0);
    CHECK(lightL.getRed()>200&&lightL.getBlue()>230&&lightR.getRed()>200&&lightR.getBlue()>230);
    bool column=true;for(const auto x:{left-6.0f,left-9.0f,right+6.0f,right+9.0f})column=column&&at(x,mid1)==juce::Colour(0xff141517);
    CHECK(column);
    // The light (rack/overview-r2.png): a blue halo fading out over the rail and in over the plate, and past it the rail
    // and the plate as they are unselected, so no band widens the plate.
    const auto blue=[](juce::Colour c){return (int)c.getBlue()-(int)c.getRed();};
    const auto luma=[](juce::Colour c){return 0.2126*c.getRed()+0.7152*c.getGreen()+0.0722*c.getBlue();};
    const auto haloL=at(left-5.5f,mid0),haloR=at(right+5.5f,mid0),plateL=at(left+4.5f,mid0),plateR=at(right-4.5f,mid0);
    std::cout<<"  the light beside Ember Reed: rail "<<haloL.toDisplayString(false)<<" and "<<haloR.toDisplayString(false)<<", plate "
             <<plateL.toDisplayString(false)<<" and "<<plateR.toDisplayString(false)<<'\n';
    CHECK(blue(haloL)>=50&&blue(haloR)>=50&&luma(haloL)>luma(at(left-8.5f,mid0))&&luma(haloR)>luma(at(right+8.5f,mid0))&&blue(plateL)>=30&&blue(plateR)>=30);
    r.stack->select(r.ids()[1]);pump();
    const auto unselected=r.body();
    bool past=true;for(const auto x:{left-12.5f,right+12.5f,left+17.5f,right-17.5f})past=past&&at(x,mid0)==unselected.getPixelAt((int)x,(int)mid0);
    CHECK(past);
    r.house->setFolded(true);pump();
    // 1 + 3 + 2 + 1 + 2 units, then the space's 1U.
    CHECK(numbered(10));
}
// Folding (the rack stage spec: The rails, A rack unit): a rack's triangle folds it to its 1U strip and back, writing
// nothing; the units under it move up; focus inside the rack goes to the triangle; a new rack starts unfolded. Glass
// Current is the house-plate copy (its faceplate's strip is stack_glass_strip's).
void stackFolds()
{
    std::cout<<"SECTION stack_folds\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);auto* press=r.unit(2);
    CHECK(ember!=nullptr&&glass!=nullptr&&press!=nullptr);
    if(ember==nullptr||glass==nullptr||press==nullptr)return;
    const auto u=r.stack->unitHeight();
    const auto before=r.revisions();const auto pressTop=r.at(*press).getY();
    CHECK(!glass->isFolded()&&glass->units()==2&&glass->fold.getComponentID()=="rack.fold"&&glass->fold.getTitle()=="Fold Glass Current");
    pressRail(glass->fold);pump();
    CHECK(glass->isFolded()&&glass->units()==1&&glass->fold.getTitle()=="Unfold Glass Current"&&std::abs(r.at(*glass).getHeight()-u)<=1.0f
          &&std::abs(r.at(*press).getY()-(pressTop-u))<=1.0f&&r.revisions()==before);
    CHECK(!glass->up.isVisible()&&!glass->down.isVisible()&&!glass->operations.isVisible()&&!glass->row.isVisible());
    ember->operations.grabKeyboardFocus();
    CHECK(ember->operations.hasKeyboardFocus(false));
    pressRail(ember->fold);pump();
    CHECK(ember->isFolded()&&juce::Component::getCurrentlyFocusedComponent()==&ember->fold);
    ember->fold.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    CHECK(!ember->isFolded()&&ember->operations.isVisible()&&r.revisions()==before);
    // A fourth rack, a second Ember Reed on synth bank 3, starts unfolded; Glass Current stays folded.
    placeRack(r.p,0,{RackRole::synth,masterChain,2},{{0,2},{1,5}});r.refresh();pump();
    CHECK(r.unit(1)==glass&&glass->isFolded()&&r.unit(2)!=nullptr&&r.unit(2)!=press&&!r.unit(2)->isFolded()&&r.unit(3)==press);
}
// The house plate's AIR mark, the wordmark's letters: printed ivory under the identity block's keys of a 2U plate, and
// carved smaller into a folded strip, as far from its right end as the name's tape is from its left, while it clears
// the LED. A maker's rack has none, on its faceplate or on the fallback strip. Glass Current is the house-plate copy and
// Thread Press a faceplate without a strip. With --images: stack-brand.png, stack-brand-folded.png and
// stack-brand-narrow.png.
void stackBranding()
{
    std::cout<<"SECTION stack_branding\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    auto* glass=r.unit(1);auto* press=r.unit(2);
    CHECK(glass!=nullptr&&press!=nullptr&&glass->face()==nullptr&&press->face()!=nullptr);
    if(glass==nullptr||press==nullptr)return;
    const auto k=r.stack->scale();
    const auto ivory=[](const juce::Image& img,juce::Rectangle<int> box)
    {
        int n=0;
        for(int y=box.getY();y<box.getBottom();++y)for(int x=box.getX();x<box.getRight();++x)
        {const auto p=img.getPixelAt(x,y);n+=std::abs(p.getRed()-0xee)<=10&&std::abs(p.getGreen()-0xea)<=10&&std::abs(p.getBlue()-0xe0)<=10&&p.getRed()-p.getBlue()>=8?1:0;}
        return n;
    };
    const auto plate=snap(*glass);
    const auto printed=ivory(plate,{96,222,284,50}),elsewhere=ivory(plate,{0,0,96,plate.getHeight()})+ivory(plate,{96,0,1451,222});
    std::cout<<"  printed: "<<printed<<" ivory px under the keys, "<<elsewhere<<" elsewhere\n";
    CHECK(printed>300&&elsewhere==0);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-brand.png"),plate));
    // Carved: darker than the plain strip beside it, with a lighter lower edge, between the strip's 150 and 300 design
    // px from its right end, the panel's or, in a narrow column, the right rail's.
    const auto carved=[&](juce::Component& c,int& dark,int& light)
    {
        const auto img=snap(c);
        const auto end=std::min(((float)studio::HouseUnit::railW+r.model.house.panelWidth)*k,(float)c.getWidth()-(float)studio::HouseUnit::railW*k);
        const auto right=juce::roundToInt(end-150.0f*k);
        const juce::Rectangle<int> mark(right-juce::roundToInt(150.0f*k),img.getHeight()/2-juce::roundToInt(20.0f*k),juce::roundToInt(150.0f*k),juce::roundToInt(40.0f*k));
        const auto plain=mark.translated(-juce::roundToInt(400.0f*k),0);
        double lo=255.0,hi=0.0;
        for(int y=plain.getY();y<plain.getBottom();++y)for(int x=plain.getX();x<plain.getRight();++x){const auto l=luma(img.getPixelAt(x,y));lo=std::min(lo,l);hi=std::max(hi,l);}
        dark=light=0;
        for(int y=mark.getY();y<mark.getBottom();++y)for(int x=mark.getX();x<mark.getRight();++x){const auto l=luma(img.getPixelAt(x,y));dark+=l<lo-6.0?1:0;light+=l>hi+6.0?1:0;}
        return img;
    };
    pressRail(glass->fold);pressRail(press->fold);pump();
    CHECK(glass->isFolded()&&press->isFolded());
    int dark=0,light=0,makerDark=0,makerLight=0;
    const auto strip=carved(*glass,dark,light);carved(*press,makerDark,makerLight);
    std::cout<<"  carved: "<<dark<<" dark and "<<light<<" light px; on Thread Press's strip "<<makerDark<<" and "<<makerLight<<'\n';
    CHECK(dark>150&&light>20&&makerDark==0&&makerLight==0);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-brand-folded.png"),strip));
    r.stack->setSize(1143,754);pump();
    CHECK(r.stack->narrow()&&glass->getWidth()==1143&&glass->isFolded());
    const auto narrow=carved(*glass,dark,light);
    std::cout<<"  carved in the narrow column: "<<dark<<" dark and "<<light<<" light px\n";
    CHECK(dark>150&&light>20);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-brand-narrow.png"),narrow));
}
// The marks (the rack stage spec: A rack unit, The divider): the folded strip's LED lit while the rack's banks are on;
// AI: KEEP across the left ear while one is AI-locked, folded or not; the divider a label taking no clicks or focus, shown
// while a master FX rack is placed; a screw in each ear at the rails' holes. With --images: stack-marks.png, the body with
// Glass Current locked and folded and Thread Press bypassed and folded. Glass Current is the house-plate copy, whose strip
// is the house's.
void stackMarks()
{
    std::cout<<"SECTION stack_marks\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);auto* press=r.unit(2);
    CHECK(ember!=nullptr&&glass!=nullptr&&press!=nullptr);
    if(ember==nullptr||glass==nullptr||press==nullptr)return;
    const auto k=r.stack->scale();
    // The LED, 40 design px right of the name tape, which starts at 194 and is at least 180 wide.
    pressRail(press->fold);pump();
    const auto tapeW=std::max(180.0f,studio::house::TapeArt::widthFor(r.model.house,"THREAD PRESS",54.0f));
    // Its lens, as programmerButtons reads ANALOG's: the unlit lens is the PROGRAMMER's grey dome.
    const auto ledLuma=[&]{return meanLuma(snap(*press),Rect(8.0f,8.0f).withCentre(Pt((194.0f+tapeW+40.0f)*k,(float)press->getHeight()*0.5f)));};
    const auto lit=ledLuma();
    CHECK(press->isFolded()&&press->on()&&lit>150.0);
    // Effect bank 4's bypass, by RackReview's rule.
    auto d=document(r.p);d.values[572+23*3]=1.0f;CHECK(edit(r.p,d));r.refresh();pump();
    const auto dark=ledLuma();
    CHECK(!press->on()&&lit>dark+40.0);
    std::cout<<"  LED: on "<<lit<<", bypassed "<<dark<<'\n';
    // AI: KEEP across Glass Current's left ear while its synth bank is AI-locked, unfolded and folded.
    const auto yellow=[&](juce::Component& c)
    {
        const auto img=snap(c);int n=0;
        for(int y=0;y<juce::roundToInt(90.0f*k);++y)for(int x=0;x<juce::roundToInt(190.0f*k);++x)
        {const auto p=img.getPixelAt(x,y);n+=p.getRed()>180&&p.getGreen()>150&&p.getBlue()<120?1:0;}
        return n;
    };
    CHECK(!glass->kept()&&yellow(*glass)==0);
    d=document(r.p);d.synthLocks[1]=true;CHECK(edit(r.p,d));r.refresh();pump();
    const auto taped=yellow(*glass);
    CHECK(glass->kept()&&taped>500);
    pressRail(glass->fold);pump();
    CHECK(glass->isFolded()&&yellow(*glass)>500);
    std::cout<<"  AI: KEEP: "<<taped<<" yellow px\n";
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-marks.png"),r.body()));
    // A screw in each ear at the rails' holes, 8.75 mm in from each end of the panel: on the top and bottom rows of a 2U
    // plate (6.35 mm and 82.55 mm) and on the middle row of a 1U strip (22.225 mm), each with a recess darker than the face.
    const auto mm=r.model.house.unitHeight/44.45f,railW=(float)studio::HouseUnit::railW;
    const auto low=[](const juce::Image& img,Rect box)
    {
        double lo=255.0;const auto b=box.getSmallestIntegerContainer().getIntersection(img.getBounds());
        for(int y=b.getY();y<b.getBottom();++y)for(int x=b.getX();x<b.getRight();++x)lo=std::min(lo,luma(img.getPixelAt(x,y)));
        return lo;
    };
    const auto screwed=[&](juce::Component& c,std::initializer_list<float> rows)
    {
        const auto img=snap(c);bool ok=true;
        for(const auto side:{-1.0f,1.0f})
            for(const auto row:rows)
            {
                const auto x=side<0.0f?railW+8.75f*mm:railW+r.model.house.panelWidth-8.75f*mm;
                const Pt centre(x*k,row*mm*k),face=centre+Pt(-side*24.0f,0.0f);
                const auto screw=low(img,Rect(16.0f,16.0f).withCentre(centre)),plate=low(img,Rect(8.0f,8.0f).withCentre(face));
                ok=ok&&screw+15.0<plate;
                std::cout<<"  screw at "<<centre.x<<','<<centre.y<<": "<<screw<<" against the face's "<<plate<<'\n';
            }
        return ok;
    };
    CHECK(screwed(*glass,{22.225f}));
    // Glass Current's 2U plate, unlocked so AI: KEEP leaves its top screw clear.
    d=document(r.p);d.synthLocks[1]=false;CHECK(edit(r.p,d));r.refresh();pressRail(glass->fold);pump();
    CHECK(!glass->isFolded()&&screwed(*glass,{6.35f,82.55f}));
    // The divider, then gone with the shared master's last FX rack.
    auto* divider=r.divider();
    bool self=true,children=true;
    if(divider!=nullptr)divider->getInterceptsMouseClicks(self,children);
    CHECK(divider!=nullptr&&divider->isVisible()&&divider->getTitle()=="SHARED MASTER - AFFECTS ALL SYNTHS"&&!divider->getWantsKeyboardFocus()&&!self&&!children);
    if(divider!=nullptr){auto* h=divider->getAccessibilityHandler();CHECK(h!=nullptr&&h->getRole()==juce::AccessibilityRole::label);}
    if(divider!=nullptr)CHECK(screwed(*divider,{22.225f}));
    RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=press->id();
    SoundDocument now;std::uint64_t revision=0;
    CHECK(r.p.trySoundSnapshot(now,revision)&&r.p.applyRackOperation(remove,revision)==RackCandidateStatus::ready);
    r.refresh();pump();
    CHECK(divider!=nullptr&&!divider->isVisible()&&r.unit(2)==nullptr);
}
// Sizes (the rack stage spec: Sizes and the other units): the house unit at the stack's scale, a rack 2U and its strip
// 1U, a rack's row panel at (400, its top, 1045, its height); beside a side unit the whole stack scales, and Ember Reed's
// AIR is still the component under its pivot. Glass Current is the house-plate copy, whose row shows. With --images:
// stack-drawn.png, the body with the drawn racks, Glass Current folded.
void stackSizes()
{
    std::cout<<"SECTION stack_sizes\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);
    CHECK(ember!=nullptr&&glass!=nullptr);
    if(ember==nullptr||glass==nullptr)return;
    const auto u=r.stack->unitHeight();
    CHECK(r.house->getTransform().mat00==r.stack->scale()&&r.house->getTransform().mat11==r.stack->scale());
    const auto unit=r.at(*glass),row=r.at(glass->row);
    CHECK(std::abs(unit.getHeight()-2.0f*u)<=1.0f&&unit.getWidth()==1547.0f);
    CHECK(row.getX()==400.0f&&row.getY()==unit.getY()&&row.getWidth()==1045.0f&&row.getHeight()==unit.getHeight());
    pressRail(glass->fold);pump();
    CHECK(std::abs(r.at(*glass).getHeight()-u)<=1.0f);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-drawn.png"),r.body()));
    r.stack->setTransform(juce::AffineTransform::scale(1143.0f/1547.0f));pump();
    const auto box=r.holder.getLocalArea(r.stack.get(),r.stack->getLocalBounds().toFloat());
    const auto tall=r.holder.getLocalArea(ember,ember->getLocalBounds().toFloat()).getHeight();
    auto* air=ember->face()!=nullptr?ember->face()->face.slider<studio::house::Knob>("air"):nullptr;
    // Ember Reed's 3U: 3 x 136.86 x 1143 / 1547.
    CHECK(std::abs(box.getWidth()-1143.0f)<=0.5f&&std::abs(tall-303.4f)<=1.0f&&air!=nullptr);
    if(air!=nullptr){const auto c=r.holder.getLocalPoint(air,local(*air,air->element.pivot));CHECK(r.holder.getComponentAt(c.roundToInt())==air);}
}
// Selection (the rack stage spec: Selection; decision 11): the first rack when none is; a press anywhere in a rack, its
// folded strip included, or the focus entering it; not the divider or the house unit; the light beside the selected rack.
void stackSelection()
{
    std::cout<<"SECTION stack_selection\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    const auto ids=r.ids();
    CHECK(ids.size()==3&&r.unit(2)!=nullptr);
    if(ids.size()!=3||r.unit(2)==nullptr)return;
    const auto ember=ids[0],glass=ids[1],press=ids[2];
    CHECK(r.stack->selected()==ember&&r.selections==std::vector<juce::Uuid>{ember});
    // Under Ember Reed's 3U, Glass Current starts below the view.
    r.stack->reveal(glass);pump();
    pressThrough(*r.unit(1),{150.0f,40.0f});
    CHECK(r.stack->selected()==glass&&!r.selections.empty()&&r.selections.back()==glass);
    // The light beside Glass Current on both rails' inner edges; beside Ember Reed, the rails' edges.
    {
        const auto img=r.body();const auto k=r.stack->scale();
        const float left=(float)studio::HouseUnit::railW*k,right=((float)studio::HouseUnit::railW+r.model.house.panelWidth)*k;
        bool lit=true;
        for(const auto x:{left-1.5f,right+1.5f})
        {
            const auto on=img.getPixelAt((int)x,(int)r.at(*r.unit(1)).getCentreY()),off=img.getPixelAt((int)x,(int)r.at(*r.unit(0)).getCentreY());
            lit=lit&&luma(on)>200.0&&on.getBlue()>=on.getRed()&&luma(off)<60.0;
        }
        CHECK(lit);
    }
    if(auto* divider=r.divider())
    {
        r.stack->viewport().setViewPosition(0,(int)r.at(*divider).getY()-200);pump();
        pressThrough(*divider,{700.0f,60.0f});
        r.stack->viewport().setViewPosition(0,0);pump();
    }
    pressThrough(r.house->unit,{1000.0f,300.0f});
    CHECK(r.stack->selected()==glass);
    pressRail(r.unit(0)->fold);pump();
    pressThrough(*r.unit(0),{700.0f,60.0f});
    CHECK(r.unit(0)->isFolded()&&r.stack->selected()==ember);
    r.unit(2)->operations.grabKeyboardFocus();pump();
    CHECK(r.stack->selected()==press);
    // Thread Press, placed last, goes with an undo: the first rack is selected.
    CHECK(r.p.undoSoundEdit());
    CHECK(wait("the first rack selected after the undo",[&]{r.refresh();return r.unit(2)==nullptr&&r.stack->selected()==ember;}));
    if(r.unit(2)!=nullptr||r.stack->selected()!=ember)
    {
        const auto name=[&](juce::Uuid id){return id==ember?"Ember Reed":id==glass?"Glass Current":id==press?"Thread Press":id.isNull()?"none":"another rack";};
        auto* f=juce::Component::getCurrentlyFocusedComponent();auto* peer=r.holder.getPeer();
        std::cout<<"  after the undo: "<<r.ids().size()<<" racks placed, Thread Press's unit "<<(r.unit(2)!=nullptr?"shown":"gone")<<", selected "<<name(r.stack->selected())<<", the selections";
        for(const auto& s:r.selections)std::cout<<' '<<name(s)<<';';
        std::cout<<" the focus on "<<(f!=nullptr?f->getComponentID():juce::String("nothing"))<<", the window "<<(peer!=nullptr&&peer->isFocused()?"focused":"unfocused")<<'\n';
    }
}
// A factory design placed with an explicit allocation, as placeRack places an example.
juce::Uuid placeFactory(InstrumentProcessor& p,std::size_t n,RackDestination destination,std::vector<UnitBinding> allocation,bool reuse=false)
{
    RackOperation op;op.design=airack::test::frozenFactoryRackDesigns()[n];op.newInstance=juce::Uuid();op.destination=destination;
    op.destination.role=op.design->role;op.allocation=std::move(allocation);op.allowSlotReuse=reuse;
    SoundDocument d;std::uint64_t revision=0;
    CHECK(p.trySoundSnapshot(d,revision)&&p.applyRackOperation(op,revision)==RackCandidateStatus::ready);
    return op.newInstance;
}
// The open panel's knobs, key by key, whose hit circles overlap another's or whose boxes leave the panel at that key's
// height. A knob's box holds its image's margins, which overlap the next knob's on a squeezed row as the PROGRAMMER's
// do; its hit circle is what a click lands on.
int knobClashes(StackRig& r)
{
    auto& c=r.stack->controls();
    auto panel=r.at(c);
    const auto hit=(float)(double)r.model.programmer["knob"]["hit_r"];
    int overlaps=0,outside=0;
    for(int n=0;n<(int)c.keys().size();++n)
    {
        c.showKey(n);pump();
        panel=r.at(c);
        const auto shown=c.shownKnobs();
        for(std::size_t a=0;a<shown.size();++a)
        {
            const auto ka=r.at(*shown[a]);
            const auto ra=hit*ka.getWidth()/(float)shown[a]->getWidth();
            if(!panel.contains(ka))++outside;
            for(std::size_t b=a+1;b<shown.size();++b)
            {
                const auto kb=r.at(*shown[b]);
                if(ka.getCentre().getDistanceFrom(kb.getCentre())<ra+hit*kb.getWidth()/(float)shown[b]->getWidth()&&++overlaps<=4)
                    std::cout<<"  overlap on "<<c.keys()[(std::size_t)n]<<": "<<shown[a]->getComponentID()<<' '<<ka.toString()<<" and "<<shown[b]->getComponentID()<<' '<<kb.toString()<<'\n';
            }
        }
    }
    std::cout<<"  the panel "<<panel.toString()<<": "<<overlaps<<" overlapping hit circles, "<<outside<<" knobs outside\n";
    c.showKey(0);pump();
    return overlaps+outside;
}
// Decision 2 in the stack: a rack's drawer shows the all-controls panel over the rack's units, between the rack and the
// form, and its knobs edit the sound. Review Focus 2: the rack goes through a host recall while a panel knob is held, or
// is swapped for a design with other units. Review Focus 3: at 1080 px the shown knobs' hit circles don't overlap and
// their boxes stay in the panel. With --images: stack-drawer-common.png, -analog.png, -lfo.png and -routes.png, Copper Arc's drawer on each key.
void stack_drawer_controls()
{
    std::cout<<"SECTION stack_drawer_controls\n";
    using Keys=std::vector<juce::String>;
    {
        StackRig r;
        drawnRacks(r.p,true);r.refresh();pump();
        const auto ids=r.ids();
        CHECK(ids.size()==3&&r.unit(1)!=nullptr);
        if(ids.size()!=3||r.unit(1)==nullptr)return;
        auto* glassUnit=r.unit(1);
        click(glassUnit->operations);
        auto& c=r.stack->controls();
        CHECK(r.stack->drawerRack()==ids[1]&&c.isShowing());
        CHECK(c.keys()==(Keys{"COMMON","WAVETABLE","LFO 1/2","ROUTES","FX 2","FX 3"}));
        std::vector<std::size_t> reach;
        for(const auto& u:studio::panelUnits(document(r.p).personalities->instances[1]))for(const auto i:unitReach(u.module,u.bank,u.revision))reach.push_back(i);
        std::sort(reach.begin(),reach.end());
        CHECK(c.parameters()==reach);
        const auto panel=r.at(c);
        CHECK(std::abs(panel.getY()-r.at(*glassUnit).getBottom())<=1.0f&&std::abs(panel.getBottom()-r.at(r.drawer).getY())<=1.0f);
        CHECK(knobClashes(r)==0);
        c.showKey(1);
        auto* k=c.knob(160);
        CHECK(k!=nullptr&&k->isShowing());
        if(k==nullptr)return;
        const auto before=r.p.control(160).getValue();
        r.reported.clear();
        k->setValue(k->getNormalisableRange().convertFrom0to1(before<0.5f?0.8:0.2),juce::sendNotificationSync);pump();
        std::cout<<"  knob 160 from "<<before<<" to "<<r.p.control(160).getValue()<<"; said '"<<r.reported.joinIntoString(" | ")<<"'\n";
        CHECK(std::abs(r.p.control(160).getValue()-before)>0.1f&&r.reported.isEmpty());
    }
    // Review Focus 2, the recall: Glass Current's drawer open and a panel knob held, then the host recalls the state
    // from before its placement. The drawer closes, the knob goes, its gesture ends once and the value is the recalled one.
    {
        StackRig r;
        placeRack(r.p,0,{},{{0,0},{1,0}},true);
        juce::MemoryBlock before;r.p.getStateInformation(before);
        placeRack(r.p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}},false,true);
        r.refresh();pump();
        const auto ids=r.ids();
        CHECK(ids.size()==2&&r.unit(1)!=nullptr);
        if(ids.size()!=2||r.unit(1)==nullptr)return;
        click(r.unit(1)->operations);
        auto& c=r.stack->controls();
        const auto level=idx("synth.2.level");
        juce::Component::SafePointer<studio::ProgrammerKnob> knob(c.knob(level));
        CHECK(knob!=nullptr&&knob->isShowing());
        if(knob==nullptr)return;
        const auto centre=knob->getLocalBounds().toFloat().getCentre();
        knob->mouseDown(event(*knob,centre,juce::ModifierKeys::leftButtonModifier,1,false));
        knob->mouseDrag(event(*knob,centre-Pt(0.0f,10.0f),juce::ModifierKeys::leftButtonModifier,1,true,centre));
        const bool held=!r.p.beginParameterGesture(level);
        CHECK(held);
        if(!held)r.p.endParameterGesture(level);
        r.p.setStateInformation(before.getData(),(int)before.getSize());
        const auto recalled=r.p.control(level).getValue();
        CHECK(wait("Glass Current retired",[&]{r.refresh();return r.unit(1)==nullptr;}));
        pump();
        CHECK(r.stack->drawerRack().isNull()&&knob==nullptr&&!c.isVisible());
        CHECK(r.p.beginParameterGesture(level)&&r.p.endParameterGesture(level));
        CHECK(r.p.control(level).getValue()==recalled);
    }
    // Review Focus 2, the swap: Ember Reed's drawer open, Ember Reed swapped for Prism Relay; the drawer stays open on
    // Prism Relay's units.
    {
        StackRig r;
        const auto ember=placeRack(r.p,0,{},{{0,0},{1,0}},true);
        r.refresh();pump();
        r.stack->openDrawer(ember);pump();
        auto& c=r.stack->controls();
        CHECK(c.keys()==(Keys{"COMMON","ANALOG","LFO 1/2","ROUTES","FX 1"}));
        RackOperation swap;swap.kind=RackOperationKind::swap;swap.selected=ember;swap.design=airack::test::frozenFactoryRackDesigns()[1];swap.allowSlotReuse=true;
        SoundDocument d;std::uint64_t revision=0;
        const auto status=r.p.trySoundSnapshot(d,revision)?r.p.applyRackOperation(swap,revision):RackCandidateStatus::stale;
        std::cout<<"  the swap: status "<<(int)status<<'\n';
        CHECK(status==RackCandidateStatus::ready);
        r.refresh();pump();
        CHECK(r.stack->drawerRack()==ember&&c.isShowing()&&c.keys()==(Keys{"COMMON","WAVETABLE","WARMTH","LFO 1/2","ROUTES"}));
    }
    // Review Focus 3 at 1080 px, then the snapshots at 1547: Copper Arc on bank 0 by reuse, its drawer open on each key.
    {
        StackRig r;
        const auto copper=placeFactory(r.p,0,{},{{0,0}},true);
        r.refresh();pump();
        r.stack->setSize(1080,754);pump();
        r.stack->openDrawer(copper);pump();
        auto& c=r.stack->controls();
        CHECK(r.stack->drawerRack()==copper&&c.isShowing()&&c.keys().size()==4);
        CHECK(knobClashes(r)==0&&std::abs(r.at(c).getWidth()-1018.0f)<=1.0f);
        r.stack->setSize(1547,754);pump();
        r.stack->openDrawer(copper);pump();
        std::cout<<"  at 1547 px: the drawer on "<<(r.stack->drawerRack()==copper?"Copper Arc":r.stack->drawerRack().isNull()?"nothing":"another rack")
                 <<", keys "<<juce::StringArray(c.keys().data(),(int)c.keys().size()).joinIntoString(", ")<<'\n';
        CHECK(r.stack->drawerRack()==copper&&c.isShowing());
        CHECK(r.status().isEmpty());
        const char* names[]{"common","analog","lfo","routes"};
        for(int n=0;n<4&&r.stack->drawerRack()==copper;++n)
        {
            c.showKey(n);pump();
            const auto span=r.at(*r.unit(0)).getUnion(r.at(r.drawer)).getSmallestIntegerContainer();
            if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-drawer-"+juce::String(names[n])+".png"),r.body().getClippedImage(span)));
        }
    }
}
// Decision 3: a bank no rack holds is a generic rack, lettered with its bank and module, with OPERATIONS alone; it is
// never selected for the AI and never dragged. Its drawer holds the bank's panel and no operation: racks are placed from
// the LIBRARY (the studio feedback spec's decision 1). Review Focus 5: showing, opening and closing a generic rack writes nothing. With
// --images: stack-generic-rack.png, synth 1's generic rack with its drawer open.
void stack_generic_racks()
{
    std::cout<<"SECTION stack_generic_racks\n";
    using Keys=std::vector<juce::String>;
    StackRig r;
    const auto generic=[&](const char* id){return dynamic_cast<RackStackUnit*>(find(r.stack->body(),id));};
    auto* g=generic("rack.bank.synth.1");
    CHECK(g!=nullptr&&r.stack->units().size()==1&&r.unit(0)==nullptr);
    if(g==nullptr)return;
    const auto bare=g->id();
    std::cout<<"  synth 1: '"<<g->name()<<"', '"<<g->place()<<"', '"<<g->getDescription()<<"'\n";
    CHECK(g->name()=="SYNTH 1 ANALOG"&&g->place()=="SYNTH 1"&&g->getDescription()=="SYNTH 1 ANALOG, no rack");
    CHECK(!g->up.isVisible()&&!g->down.isVisible()&&!g->row.isVisible()&&g->operations.isShowing()&&g->face()==nullptr&&g->on()&&!g->kept());
    CHECK(r.stack->selected().isNull()&&r.selections.empty());
    const auto before=r.revisions();
    r.refresh();pump();
    click(g->operations);
    CHECK(r.stack->drawerRack()==bare&&r.drawerUnder(*g)&&r.selections==std::vector<juce::Uuid>{juce::Uuid::null()});
    click(g->operations);
    CHECK(r.stack->drawerRack().isNull()&&r.revisions()==before);
    // Its drawer: synth 1's panel under its header, and no operation.
    r.stack->openDrawer(bare);pump();
    auto& c=r.stack->controls();
    CHECK(c.isShowing()&&c.keys()==(Keys{"COMMON","ANALOG","LFO 1/2","ROUTES"}));
    auto* operation=widget<juce::ComboBox>(r.drawer,"rack.operation");
    auto* banks=find(r.drawer,"rack.allocation");
    auto* header=widget<juce::Label>(r.drawer,"rack.header");
    CHECK(operation!=nullptr&&banks!=nullptr&&header!=nullptr);
    if(operation==nullptr||banks==nullptr||header==nullptr)return;
    std::cout<<"  the drawer: '"<<header->getText().replace("\n"," / ")<<"', "<<operation->getNumItems()<<" operations\n";
    CHECK(operation->getNumItems()==0&&!operation->isShowing()&&!banks->isShowing()&&find(r.drawer,"rack.name")==nullptr
          &&header->getText()=="SYNTH 1 ANALOG | no rack\nSYNTH 1");
    CHECK(r.status().isEmpty()&&r.revisions()==before);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-generic-rack.png"),r.body().getClippedImage(r.at(*g).getUnion(r.at(r.drawer)).getSmallestIntegerContainer())));
    // A delay on the shared master in effect bank 1: its generic rack under the divider.
    SoundDocument s;std::uint64_t revision=0;
    CHECK(r.p.trySoundSnapshot(s,revision)&&r.p.applyRackEdit({RackEditKind::addEffect,0,masterChain,0,EffectKind::delay},revision)==RackEditResult::applied);
    r.refresh();pump();
    auto* fx=generic("rack.bank.fx.1");
    CHECK(fx!=nullptr&&r.divider()!=nullptr&&r.divider()->isVisible());
    if(fx==nullptr||r.divider()==nullptr)return;
    CHECK(fx->name()=="FX 1 DELAY"&&fx->place()=="SHARED MASTER"&&r.at(*fx).getY()>=r.at(*r.divider()).getBottom()-1.0f);
    // Pressed and moved 20 px, it starts no drag.
    r.stack->viewport().setViewPosition(0,std::max(0,fx->getY()-200));pump();
    Pointer m{*r.stack};
    const auto at=r.stack->getLocalPoint(fx,Pt(150.0f,20.0f));
    m.press(at);m.move(at+Pt(0.0f,-20.0f));
    CHECK(!r.stack->dragging());
    m.release(at+Pt(0.0f,-20.0f));pump();
}
// Decision 9: a proposal changing what only the drawer shows (a parameter in the rack's reach, not locked, on neither its
// row nor its faceplate) outlines the rack's OPERATIONS while its drawer is closed; open, the panel marks it. With
// --images: stack-operations-outlined.png, Glass Current's unit with the ring, and stack-drawer-marks.png, its drawer on
// ROUTES at route 9's page.
void stack_hidden_proposals()
{
    std::cout<<"SECTION stack_hidden_proposals\n";
    // Pixels at least 200 in each channel on a band round OPERATIONS' bounds expanded by 3 px.
    const auto ring=[](StackRig& r,RackStackUnit& u)
    {
        const auto img=r.body();const auto box=r.at(u.operations);const auto outer=box.expanded(4.5f),inner=box.expanded(1.5f);
        int n=0;
        for(int y=(int)outer.getY();y<(int)outer.getBottom();++y)
            for(int x=(int)outer.getX();x<(int)outer.getRight();++x)
            {
                const Pt p((float)x+0.5f,(float)y+0.5f);
                const auto c=img.getPixelAt(x,y);
                if(outer.contains(p)&&!inner.contains(p)&&std::min({c.getRed(),c.getGreen(),c.getBlue()})>=200)++n;
            }
        return n;
    };
    // The live sound with each of `changed` moved across its range.
    const auto candidate=[](InstrumentProcessor& p,std::initializer_list<std::size_t> changed)
    {
        auto c=document(p);
        for(const auto i:changed)c.values[i]=c.values[i]<0.5f?0.8f:0.2f;
        return c;
    };
    {
        StackRig r;
        drawnRacks(r.p,true);r.refresh();pump();
        const auto ids=r.ids();
        CHECK(ids.size()==3&&r.unit(2)!=nullptr);
        if(ids.size()!=3||r.unit(2)==nullptr)return;
        auto* ember=r.unit(0);auto* glass=r.unit(1);auto* press=r.unit(2);
        const auto propose=[&](std::initializer_list<std::size_t> changed){const auto c=candidate(r.p,changed);r.stack->setProposal(&c);pump();};
        const auto unmarked=ring(r,*glass);
        propose({376});
        const auto marked=ring(r,*glass);
        std::cout<<"  376: hidden on Ember Reed "<<ember->hiddenProposal()<<", Glass Current "<<glass->hiddenProposal()<<", Thread Press "
                 <<press->hiddenProposal()<<"; ring pixels "<<unmarked<<" without, "<<marked<<" with\n";
        CHECK(glass->hiddenProposal()&&!ember->hiddenProposal()&&!press->hiddenProposal()&&glass->proposed());
        CHECK(unmarked==0&&marked>=200&&glass->getDescription().endsWith("\nAI proposes changes in the drawer"));
        if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-operations-outlined.png"),r.body().getClippedImage(r.at(*glass).getSmallestIntegerContainer())));
        // Glass Current's first row control alone: the row marks it.
        const auto first=firstControl(document(r.p),1);
        CHECK(first.has_value());
        if(first)propose({*first});
        CHECK(!glass->hiddenProposal());
        // Ember Reed's faceplate shows 39, but not 296, synth 1's route 9 depth.
        propose({39});
        CHECK(!ember->hiddenProposal());
        propose({296});
        CHECK(ember->hiddenProposal());
        // Nor is a parameter in its reach that its faceplate alone shows.
        std::vector<std::size_t> reach;
        for(const auto& u:studio::panelUnits(document(r.p).personalities->instances[0]))for(const auto i:unitReach(u.module,u.bank,u.revision))reach.push_back(i);
        const auto onFace=ember->face()!=nullptr?ember->face()->parameters():std::vector<std::size_t>{},onRow=ember->row.parameters();
        const auto faceOnly=std::find_if(onFace.begin(),onFace.end(),[&](std::size_t p)
            {return std::find(onRow.begin(),onRow.end(),p)==onRow.end()&&std::find(reach.begin(),reach.end(),p)!=reach.end();});
        CHECK(faceOnly!=onFace.end());
        if(faceOnly!=onFace.end()){std::cout<<"  on Ember Reed's faceplate alone: "<<*faceOnly<<'\n';propose({*faceOnly});CHECK(!ember->hiddenProposal());}
        // Glass Current's drawer open on 376: no ring; the panel marks 376 and outlines ROUTES' key, then shows it on
        // route 9's page.
        propose({376});
        click(glass->operations);
        auto& c=r.stack->controls();
        const auto marks=c.marks();
        CHECK(r.stack->drawerRack()==ids[1]&&ring(r,*glass)==0&&glass->hiddenProposal());
        CHECK(std::any_of(marks.begin(),marks.end(),[](const auto& m){return m.index==376;})&&c.outlinedKeys()==std::vector<int>{3}&&c.keys()[3]=="ROUTES");
        for(int n=0;n<3;++n)c.showKey(3);
        pump();
        auto* knob=c.knob(376);
        CHECK(c.routesPage()==2&&knob!=nullptr&&knob->isShowing()&&c.outlinedKeys().empty());
        CHECK(r.status().isEmpty());
        if(output!=juce::File{})
            CHECK(writePng(output.getChildFile("stack-drawer-marks.png"),r.body().getClippedImage(r.at(*glass).getUnion(r.at(r.drawer)).getSmallestIntegerContainer())));
    }
    // The default sound's generic rack: 39 is behind its drawer.
    {
        StackRig r;
        auto* g=dynamic_cast<RackStackUnit*>(find(r.stack->body(),"rack.bank.synth.1"));
        CHECK(g!=nullptr);
        if(g==nullptr)return;
        const auto c=candidate(r.p,{39});
        r.stack->setProposal(&c);pump();
        CHECK(g->hiddenProposal());
    }
}
// The drawer (the rack stage spec: The drawer; decision 10): OPERATIONS opens it under its rack, lit, and closes it; one
// at a time; another rack selected closes it and cancels its draft; folding closes it; an APPLY that places a rack
// selects it and reports. Review Focus 1: the selected rack goes while its drawer is open and a control is held. Review
// Focus 4: the strip-only view keeps the drawer and its draft. Glass Current is the house-plate copy, whose OPERATIONS
// key and row are the house plate's. With --images: stack-drawer.png, Glass Current's drawer.
void stackDrawer()
{
    std::cout<<"SECTION stack_drawer\n";
    {
        StackRig r;
        drawnRacks(r.p,true);r.refresh();pump();
        const auto ids=r.ids();
        CHECK(ids.size()==3&&r.unit(2)!=nullptr&&r.divider()!=nullptr);
        if(ids.size()!=3||r.unit(2)==nullptr||r.divider()==nullptr)return;
        const auto ember=ids[0],glass=ids[1];
        auto* glassUnit=r.unit(1);
        const auto u=r.stack->unitHeight();const auto dividerTop=r.at(*r.divider()).getY();
        click(glassUnit->operations);
        CHECK(r.stack->drawerRack()==glass&&glassUnit->operations.getToggleState()&&r.stack->selected()==glass&&r.drawer.isShowing());
        // The drawer spans whole units under its rack: the all-controls panel at its page's height, then the form, 500 px
        // tall while its difference box hides and 668 px once it shows; the divider under it moves with it (F14.5).
        const auto box=r.at(r.drawer),panel=r.at(r.stack->controls());
        const auto spans=[&](float page,float form){
            const auto units=std::ceil((page+form)/u);const auto b=r.at(r.drawer),p=r.at(r.stack->controls());const auto d=r.at(*r.divider()).getY();
            const bool ok=std::abs(b.getY()-(p.getY()+page))<=1.0f&&std::abs(b.getBottom()-(p.getY()+units*u))<=1.0f&&std::abs(d-(dividerTop+units*u))<=1.0f;
            std::cout<<"  the panel "<<p.toString()<<", the drawer "<<b.toString()<<", the divider at "<<d<<"; "<<page<<" + "<<form<<" px in "<<units<<"U\n";
            return ok;
        };
        CHECK(std::abs(panel.getY()-r.at(*glassUnit).getBottom())<=1.0f&&box.getX()==31.0f&&box.getRight()==1516.0f&&spans(345.0f,500.0f));
        // One face over the panel and the form: across the form's top, 40 px inside the plate's right edge, the tone holds;
        // the form's engraved line at its top, 700 px in.
        {
            const auto image=r.body();const auto top=box.getY(),x=box.getRight()-40.0f;
            const auto above=meanLuma(image,Rect(x-24.0f,top-6.0f,24.0f,3.0f)),below=meanLuma(image,Rect(x-24.0f,top+4.0f,24.0f,3.0f));
            const int lx=juce::roundToInt(box.getX())+700,ly=juce::roundToInt(top);
            const auto line=luma(image.getPixelAt(lx,ly)),under=luma(image.getPixelAt(lx,ly+1));
            std::cout<<"  across the form's top: "<<above<<" above, "<<below<<" below; its line "<<line<<" over "<<under<<'\n';
            CHECK(std::abs(above-below)<=3.0&&line>=180.0&&under<=120.0);
        }
        auto* operation=widget<juce::ComboBox>(r.drawer,"rack.operation");
        auto* header=widget<juce::Label>(r.drawer,"rack.header");
        auto* apply=widget<juce::Button>(r.drawer,"rack.apply");
        if(operation==nullptr||header==nullptr||apply==nullptr)return;
        juce::Array<int> items;for(int n=0;n<operation->getNumItems();++n)items.add(operation->getItemId(n));
        const auto d=document(r.p);
        CHECK(items==juce::Array<int>({4,5,6,7,8})&&header->getText()==rackHeaderText(d,d.personalities->instances[1])&&find(r.drawer,"rack.advanced")!=nullptr);
        // Decision 15: the open drawer's OPERATIONS lit, against Ember Reed's.
        const auto lens=Rect(8.0f,8.0f).withCentre({42.0f,30.0f-13.84f});
        const auto lit=meanLuma(snap(glassUnit->operations),lens),unlit=meanLuma(snap(r.unit(0)->operations),lens);
        CHECK(lit>unlit+60.0);
        std::cout<<"  OPERATIONS lens: "<<lit<<" lit, "<<unlit<<" unlit\n";
        CHECK(r.status().isEmpty());
        if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-drawer.png"),r.body()));
        // CANCEL DRAFT shows the difference box and the drawer grows to hold it; WAVETABLE's two rows push the form down.
        operation->setSelectedId(5,juce::sendNotificationSync);click(r.drawer,"rack.cancel");pump();
        CHECK(spans(345.0f,668.0f));
        r.stack->controls().showKey(1);pump();
        CHECK(spans(601.0f,668.0f));
        r.stack->controls().showKey(0);click(r.drawer,"rack.cancel");pump();
        click(glassUnit->operations);
        CHECK(r.stack->drawerRack().isNull()&&!glassUnit->operations.getToggleState()&&!r.drawer.isVisible());
        // One at a time.
        click(glassUnit->operations);click(r.unit(0)->operations);
        CHECK(r.stack->drawerRack()==ember&&r.stack->selected()==ember&&!glassUnit->operations.getToggleState()&&r.unit(0)->operations.getToggleState());
        // An operation chosen, then Glass Current, under the drawer and the view, scrolled to and pressed: the drawer
        // closes.
        operation->setSelectedId(5,juce::sendNotificationSync);
        r.stack->reveal(glass);pump();
        CHECK(r.inView(*glassUnit)&&r.stack->selected()==ember);
        pressThrough(*glassUnit,{150.0f,40.0f});
        CHECK(r.stack->selected()==glass&&r.stack->drawerRack().isNull());
        r.stack->openDrawer(ember);pump();
        CHECK(r.stack->drawerRack()==ember&&r.stack->selected()==ember&&apply->isEnabled());
        // Folding the rack closes its drawer.
        pressRail(r.unit(0)->fold);pump();
        CHECK(r.stack->drawerRack().isNull()&&!r.drawer.isVisible());
        pressRail(r.unit(0)->fold);pump();
        // A duplicate's APPLY selects the copy, closes the drawer and reports; a REMOVE's takes the copy away.
        r.stack->openDrawer(ember);pump();
        operation->setSelectedId(5,juce::sendNotificationSync);click(r.drawer,"rack.apply");pump();
        const auto copies=r.ids();const auto copy=copies.size()==4?copies.back():juce::Uuid::null();
        CHECK(!copy.isNull()&&r.stack->selected()==copy&&r.stack->unit(copy)!=nullptr&&r.stack->drawerRack().isNull()
              &&r.reported.contains("Applied one reviewed rack transaction"));
        r.stack->openDrawer(copy);pump();
        operation->setSelectedId(7,juce::sendNotificationSync);click(r.drawer,"rack.apply");pump();
        CHECK(r.ids().size()==3&&r.stack->unit(copy)==nullptr&&r.stack->drawerRack().isNull()&&r.stack->selected()==ember);
    }
    // Review Focus 1: Glass Current's drawer open and its first control held with the focus on it, then the host recalls
    // the state from before its placement (an undo waits for the gesture). The drawer closes, Ember Reed is selected, the
    // gesture ends once, and the focus is not in the retired unit.
    {
        StackRig r;
        placeRack(r.p,0,{},{{0,0},{1,0}},true);
        juce::MemoryBlock before;r.p.getStateInformation(before);
        placeRack(r.p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}},false,true);
        r.refresh();pump();
        const auto ids=r.ids();const auto index=firstControl(document(r.p),1);
        CHECK(ids.size()==2&&r.unit(1)!=nullptr&&index.has_value());
        if(ids.size()!=2||r.unit(1)==nullptr||!index)return;
        juce::Component::SafePointer<juce::Component> retired(r.unit(1));
        click(r.unit(1)->operations);
        auto* control=dynamic_cast<juce::Slider*>(find(*r.unit(1),"rack.focus.0"));
        CHECK(control!=nullptr&&r.stack->drawerRack()==ids[1]);
        if(control==nullptr)return;
        control->grabKeyboardFocus();pump();control->onDragStart();
        const bool held=!r.p.beginParameterGesture(*index);
        CHECK(held);
        if(!held)r.p.endParameterGesture(*index);
        r.p.setStateInformation(before.getData(),(int)before.getSize());
        CHECK(wait("Glass Current retired",[&]{r.refresh();return r.unit(1)==nullptr;}));
        pump();
        auto* focused=juce::Component::getCurrentlyFocusedComponent();
        CHECK(retired==nullptr&&r.stack->drawerRack().isNull()&&r.stack->selected()==ids[0]&&!r.drawer.isVisible()
              &&(focused==nullptr||r.stack->body().isParentOf(focused)));
        CHECK(r.p.beginParameterGesture(*index)&&r.p.endParameterGesture(*index));
    }
    // Review Focus 4, the strip-only view: Ember Reed's drawer open with an operation chosen; the view hides it and keeps it.
    {
        StackRig r;
        drawnRacks(r.p);r.refresh();pump();
        if(r.unit(0)==nullptr)return;
        click(r.unit(0)->operations);
        auto* operation=widget<juce::ComboBox>(r.drawer,"rack.operation");auto* apply=widget<juce::Button>(r.drawer,"rack.apply");
        if(operation==nullptr||apply==nullptr)return;
        operation->setSelectedId(5,juce::sendNotificationSync);
        CHECK(apply->isEnabled());
        r.house->setStripOnly(true);r.stack->setStripOnly(true);pump();
        const auto numbers=r.stack->railNumbers();
        CHECK(!r.drawer.isShowing()&&!r.unit(0)->isShowing()&&numbers.size()==1&&numbers[0].first==1);
        r.house->setStripOnly(false);r.stack->setStripOnly(false);pump();
        CHECK(r.drawerUnder(*r.unit(0))&&apply->isEnabled());
    }
}
// UP and DOWN (the rack stage spec: UP and DOWN; decision 9): one place past the next rack of the same role, applied at
// once and undoable, with the session sentence; a locked rack moves, and an insert moves with its synth rack. Review Focus 3:
// a move disables the key with the focus, which passes to the other move key. Glass Current is the house-plate copy, whose
// UP and DOWN show.
void stackMoves()
{
    std::cout<<"SECTION stack_moves\n";
    const auto order=[](InstrumentProcessor& p){std::vector<juce::Uuid> ids;for(const auto& s:rackOrder(document(p)))ids.push_back(s.id);return ids;};
    {
        StackRig r;
        drawnRacks(r.p,true);r.refresh();pump();
        const auto ids=r.ids();
        CHECK(ids.size()==3&&r.unit(2)!=nullptr);
        if(ids.size()!=3||r.unit(2)==nullptr)return;
        const auto ember=ids[0],glass=ids[1],press=ids[2];
        CHECK(!r.unit(0)->up.isEnabled()&&r.unit(0)->down.isEnabled()&&r.unit(1)->up.isEnabled()&&!r.unit(1)->down.isEnabled()
              &&!r.unit(2)->up.isEnabled()&&!r.unit(2)->down.isEnabled());
        click(r.unit(1)->up);
        CHECK(order(r.p)==std::vector<juce::Uuid>({glass,ember,press})&&r.unit(0)!=nullptr&&r.unit(0)->id()==glass
              &&!r.reported.isEmpty()&&r.reported[r.reported.size()-1]=="Moved Glass Current up");
        CHECK(!r.unit(0)->up.isEnabled()&&r.unit(0)->down.isEnabled()&&r.unit(1)->up.isEnabled()&&!r.unit(1)->down.isEnabled());
        CHECK(r.p.undoSoundEdit());
        CHECK(wait("Ember Reed first again",[&]{r.refresh();return r.unit(0)!=nullptr&&r.unit(0)->id()==ember;}));
    }
    // Glass Current AI-locked still moves; Thread Press on Ember Reed's chain moves with Ember Reed.
    {
        StackRig r;
        placeRack(r.p,0,{},{{0,0},{1,0}},true);
        placeRack(r.p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}},false,true);
        placeRack(r.p,2,{RackRole::fx,0,1},{{0,3},{1,4}});
        auto d=document(r.p);d.synthLocks[1]=true;CHECK(edit(r.p,d));r.refresh();pump();
        const auto ids=r.ids();
        CHECK(ids.size()==3&&r.unit(2)!=nullptr&&r.unit(2)->id()==ids[1]&&r.unit(2)->kept()&&r.unit(2)->up.isEnabled());
        if(ids.size()!=3||r.unit(2)==nullptr)return;
        click(r.unit(2)->up);
        CHECK(order(r.p)==std::vector<juce::Uuid>({ids[1],ids[0],ids[2]})&&r.unit(2)!=nullptr&&r.unit(2)->id()==ids[2]&&r.unit(2)->place()=="SYNTH 1 INSERT");
    }
    // Review Focus 3: the focus on Glass Current's UP; UP takes it to the top, and the focus to its DOWN.
    {
        StackRig r;
        drawnRacks(r.p,true);r.refresh();pump();
        auto* glassUnit=r.unit(1);
        CHECK(glassUnit!=nullptr);
        if(glassUnit==nullptr)return;
        glassUnit->up.grabKeyboardFocus();pump();
        CHECK(glassUnit->up.hasKeyboardFocus(false));
        click(glassUnit->up);
        CHECK(r.unit(0)==glassUnit&&!glassUnit->up.isEnabled()&&juce::Component::getCurrentlyFocusedComponent()==&glassUnit->down);
    }
}
// Scrolling (the rack stage spec: Scrolling; decisions 11 and 13): the scroll bar over the right rail's outer edge while
// the body is taller than the view; the wheel over the space scrolls the stack and over a knob turns it; a rack selected,
// its drawer opened, the focus entering it or a move brings it into view. Glass Current is the house-plate copy, whose UP
// shows.
void stackScrolls()
{
    std::cout<<"SECTION stack_scrolls\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    const auto ids=r.ids();
    CHECK(ids.size()==3&&r.unit(2)!=nullptr);
    if(ids.size()!=3||r.unit(2)==nullptr)return;
    auto& view=r.stack->viewport();auto& bar=r.stack->scrollBar();
    const auto box=bar.getBoundsInParent();
    CHECK(bar.isVisible()&&r.stack->isParentOf(&bar)&&box.getX()>=1539&&box.getRight()<=1547&&box.getY()==0&&box.getBottom()==754);
    spin(r.space,{700.0f,60.0f},-0.5f);
    const auto scrolled=view.getViewPositionY();
    CHECK(scrolled>0&&juce::roundToInt(bar.getCurrentRangeStart())==scrolled);
    auto* air=dynamic_cast<juce::Slider*>(find(*r.unit(0),"rack.focus.0"));
    CHECK(air!=nullptr);
    if(air!=nullptr)
    {
        // Within 400 ms of a rack scroll the knob under the pointer passes the wheel on and the stack keeps scrolling
        // (the studio feedback spec, decision 3); once the wheel has rested the knob turns.
        const auto before=air->getValue();const Pt centre((float)air->getWidth()*0.5f,(float)air->getHeight()*0.5f);
        spin(*air,centre,0.5f);
        CHECK(air->getValue()==before&&view.getViewPositionY()<scrolled);
        juce::Thread::sleep(450);view.setViewPosition(0,0);pump();
        spin(*air,centre,0.5f);
        CHECK(air->getValue()!=before&&view.getViewPositionY()==0);
    }
    r.stack->select(ids[2]);pump();
    CHECK(r.stack->selected()==ids[2]&&r.inView(*r.unit(2)));
    // Glass Current with its drawer is taller than the view: the view starts at its top.
    view.setViewPosition(0,0);pump();click(r.unit(1)->operations);
    CHECK(r.stack->drawerRack()==ids[1]&&std::abs((float)view.getViewPositionY()-r.at(*r.unit(1)).getY())<=1.0f);
    const auto big=r.stack->body().getHeight();
    view.setViewPosition(0,big);pump();r.unit(0)->operations.grabKeyboardFocus();pump();
    CHECK(r.stack->selected()==ids[0]&&r.inView(*r.unit(0)));
    view.setViewPosition(0,big);pump();click(r.unit(1)->up);
    CHECK(r.unit(0)!=nullptr&&r.unit(0)->id()==ids[1]&&r.inView(*r.unit(0)));
}
// Tab (the rack stage spec: Keyboard and accessibility; decision 14): the house unit, each rack with its open drawer after
// it, then the space; in a rack its triangle, its enabled move keys, OPERATIONS, then its row, and on Ember Reed's and
// Thread Press's faceplates (the maker faceplates spec: Keyboard and accessibility) its triangle, the faceplate's
// controls, then OPERATIONS; the keys' titles. Glass Current is the house-plate copy (its faceplate's order is
// stack_glass's).
void stackTabOrder()
{
    std::cout<<"SECTION stack_tab_order\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    if(r.unit(2)==nullptr)return;
    click(r.unit(1)->operations);
    const auto stops=juce::KeyboardFocusTraverser().getAllComponents(&r.holder);
    const auto owner=[&](juce::Component* c)->juce::String
    {
        if(r.house->isParentOf(c))return "house";
        for(int n=0;n<3;++n)if(auto* u=r.unit(n);u!=nullptr&&u->isParentOf(c))return "rack.unit."+juce::String(n);
        if(r.stack->controls().isParentOf(c))return "rack.controls";
        if(r.drawer.isParentOf(c))return "rack.drawer";
        if(r.space.isParentOf(c))return "rack.space";
        return "other "+c->getComponentID();
    };
    juce::StringArray owners;
    for(auto* c:stops){const auto o=owner(c);if(owners.isEmpty()||owners[owners.size()-1]!=o)owners.add(o);}
    const juce::StringArray order{"house","rack.unit.0","rack.unit.1","rack.controls","rack.drawer","rack.unit.2","rack.space"};
    CHECK(owners==order);
    if(owners!=order)std::cout<<"  owners: "<<owners.joinIntoString(", ")<<'\n';
    const auto first=[&](const juce::String& o)
    {
        juce::StringArray got;
        for(auto* c:stops)if(owner(c)==o){got.add(c->getComponentID());if(c->getComponentID()=="rack.focus.0")break;}
        return got;
    };
    CHECK(first("rack.unit.0")==juce::StringArray({"rack.fold","face.air","face.bite","face.bloom","face.attack","face.release","face.grain","face.programmer",
                                                   "rack.operations"}));
    CHECK(first("rack.unit.1")==juce::StringArray({"rack.fold","rack.up","rack.operations","rack.focus.0"}));
    CHECK(first("rack.unit.2")==juce::StringArray({"rack.fold","face.grip","face.front","face.blend","face.density","face.in","face.setup","rack.operations"}));
    auto* glass=r.unit(1);
    CHECK(glass->fold.getTitle()=="Fold Glass Current"&&glass->up.getTitle()=="Move Glass Current up"&&glass->down.getTitle()=="Move Glass Current down"
          &&glass->operations.getTitle()=="Operations on Glass Current");
}
// The paints of what lies under c while f runs, counted by a component laid over it in its parent.
int paintsOver(juce::Component& c,const std::function<void()>& f)
{
    struct Counter final:juce::Component { int paints=0;void paint(juce::Graphics&) override { ++paints; } } counter;
    auto* parent=c.getParentComponent();
    if(parent==nullptr)return -1;
    counter.setInterceptsMouseClicks(false,false);parent->addAndMakeVisible(counter);counter.setBounds(c.getBounds());
    pump(100);counter.paints=0;f();
    const int paints=counter.paints;
    parent->removeChildComponent(&counter);
    return paints;
}
// The text of the n-th label in c, depth first.
juce::String labelText(juce::Component& c,int n)
{
    std::vector<juce::Label*> labels;
    std::function<void(juce::Component&)> walk=[&](juce::Component& at){for(auto* child:at.getChildren()){if(auto* l=dynamic_cast<juce::Label*>(child))labels.push_back(l);walk(*child);}};
    walk(c);
    return n<(int)labels.size()?labels[(std::size_t)n]->getText():juce::String();
}
// Placements (the moves spec: Placements; decisions 4 and 5): an empty slot at each kind of destination, 2U, the divider it
// brings, the rails numbering through it, its dashes and letters, its role, the wheel passing it, an open drawer kept under
// its rack; a preview's identity block and inert row; the same placement again drawing nothing. With --images:
// stack-slot.png and stack-placement.png.
void stackPlacement()
{
    std::cout<<"SECTION stack_placement\n";
    // With no master FX rack, a master slot brings the divider, and takes it away again.
    {
        StackRig e;placeRack(e.p,0,{},{{0,0},{1,0}},true);e.refresh();pump();
        CHECK(e.unit(0)!=nullptr&&e.divider()!=nullptr&&!e.divider()->isVisible());
        if(e.unit(0)==nullptr||e.divider()==nullptr)return;
        e.stack->setPlacement(StackPlacement{{RackRole::fx,masterChain,1},"NEW RACK SHARED MASTER POSITION 2",{},{}});pump();
        auto* m=e.stack->placement();
        CHECK(e.divider()->isVisible()&&m!=nullptr);
        if(m!=nullptr)CHECK(std::abs(e.at(*e.divider()).getY()-e.at(*e.unit(0)).getBottom())<=1.0f&&std::abs(e.at(*m).getY()-e.at(*e.divider()).getBottom())<=1.0f);
        e.stack->setPlacement(std::nullopt);pump();
        CHECK(!e.divider()->isVisible()&&e.stack->placement()==nullptr);
    }
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    if(r.unit(2)==nullptr||r.divider()==nullptr)return;
    const auto U=[&](int n){return (float)juce::roundToInt((float)n*r.stack->unitHeight());};
    const auto slot=[&](RackDestination t,const char* letters){r.stack->setPlacement(StackPlacement{t,letters,{},{}});pump();return r.stack->placement();};
    // Before Thread Press under the divider: the slot 2U on rails 10 and 11, Thread Press moved down under it.
    auto* s=slot({RackRole::fx,masterChain,0},"NEW RACK SHARED MASTER POSITION 1");
    CHECK(s!=nullptr);
    if(s==nullptr)return;
    CHECK(s->getComponentID()=="rack.placement"&&s->getTitle()=="NEW RACK SHARED MASTER POSITION 1");
    CHECK(closeTo(r.at(*s),Rect(0.0f,U(9),1547.0f,U(11)-U(9)))&&closeTo(r.at(*r.unit(2)),Rect(0.0f,U(11),1547.0f,U(13)-U(11))));
    CHECK(r.stack->railNumbers().size()>=14);
    // Light grey dashes along the plate's place, inset 6 px; the letters in the middle.
    {
        const auto img=r.body();const int left=juce::roundToInt((float)studio::HouseUnit::railW*r.stack->scale()),top=(int)U(9);
        int dashes=0;
        for(int x=left+20;x<left+1000;++x)for(int y=top+4;y<=top+8;++y){const auto c=img.getPixelAt(x,y);dashes+=c.getRed()>170&&c.getRed()<230&&c.getBlue()>180?1:0;}
        CHECK(dashes>=300&&peakLuma(img,Rect(773.0f-200.0f,(U(9)+U(11))*0.5f-14.0f,400.0f,28.0f))>150.0);
        if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-slot.png"),img));
    }
    s=slot({RackRole::synth,masterChain,1},"NEW RACK SYNTH ORDER POSITION 2");
    CHECK(s!=nullptr&&std::abs(r.at(*s).getY()-r.at(*r.unit(0)).getBottom())<=1.0f&&std::abs(r.at(*r.unit(1)).getY()-r.at(*s).getBottom())<=1.0f);
    s=slot({RackRole::fx,1,0},"NEW RACK SYNTH 2 INSERT POSITION 1");
    CHECK(s!=nullptr&&std::abs(r.at(*s).getY()-r.at(*r.unit(1)).getBottom())<=1.0f&&std::abs(r.at(*r.divider()).getY()-r.at(*s).getBottom())<=1.0f);
    // A destination the stack cannot draw shows none, and the units go back.
    CHECK(slot({RackRole::fx,3,0},"NEW RACK SYNTH 4 INSERT POSITION 1")==nullptr&&closeTo(r.at(*r.unit(2)),Rect(0.0f,U(9),1547.0f,U(11)-U(9))));
    // Review Focus 3: an open drawer stays under its rack, the slot under the drawer.
    const auto ids=r.ids();
    r.stack->openDrawer(ids[0]);pump();
    s=slot({RackRole::fx,0,1},"NEW RACK SYNTH 1 INSERT POSITION 2");
    CHECK(s!=nullptr&&r.drawerUnder(*r.unit(0))&&std::abs(r.at(*s).getY()-r.at(r.drawer).getBottom())<=1.0f);
    r.stack->closeDrawer();pump();
    // A label that takes nothing: no focus or clicks; the wheel over it scrolls the stack; it is brought into view.
    r.stack->viewport().setViewPosition(0,0);pump();
    s=slot({RackRole::fx,masterChain,2},"NEW RACK SHARED MASTER POSITION 3");
    CHECK(s!=nullptr);
    if(s==nullptr)return;
    bool clicks=true,children=true;s->getInterceptsMouseClicks(clicks,children);
    CHECK(s->getAccessibilityHandler()!=nullptr&&s->getAccessibilityHandler()->getRole()==juce::AccessibilityRole::label&&!s->getWantsKeyboardFocus()&&!clicks&&!children&&r.inView(*s));
    r.stack->viewport().setViewPosition(0,0);pump();spin(*s,{700.0f,100.0f},-1.0f);
    CHECK(r.stack->viewport().getViewPositionY()>0);
    // A preview of the house-plate copy of Glass Current: its name in the identity block, PREVIEW: letters in the place
    // line, its row at its recipe's values.
    auto glass=airack::test::frozenExampleRackDesigns()[1];glass.id=juce::Uuid();
    const StackPlacement preview{{RackRole::synth,masterChain,2},"PREVIEW: SYNTH 3, AFTER GLASS CURRENT",rackPreviewInstance(glass),rackPreviewValues(glass)};
    r.stack->setPlacement(preview);pump();
    s=r.stack->placement();
    CHECK(s!=nullptr);
    if(s==nullptr)return;
    CHECK(s->getTitle()=="PREVIEW: SYNTH 3, AFTER GLASS CURRENT"&&s->getDescription()=="Glass Current"&&labelText(*s,0)=="GLASS CURRENT"&&labelText(*s,1)=="PREVIEW: SYNTH 3, AFTER GLASS CURRENT");
    CHECK(std::abs(r.at(*s).getY()-r.at(*r.unit(1)).getBottom())<=1.0f&&std::abs(r.at(*s).getHeight()-(U(2)))<=1.0f);
    auto* air=find(*s,"rack.focus.0");
    CHECK(air!=nullptr&&!air->getWantsKeyboardFocus()&&find(*s,"rack.up")==nullptr&&find(*s,"rack.operations")==nullptr);
    // Tab passes over it.
    bool skipped=true;for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))skipped=skipped&&!s->isParentOf(c);
    CHECK(skipped);
    if(output!=juce::File{}){r.stack->viewport().setViewPosition(0,(int)U(3));pump();CHECK(writePng(output.getChildFile("stack-placement.png"),r.body()));}
    // Idle: the same placement again draws nothing.
    CHECK(paintsOver(*s,[&]{for(int n=0;n<20;++n){r.stack->setPlacement(preview);pump(25);}})==0);
}
// The dashed outline round a folded strip's tape: the near-white pixels in the band 3 to 9 px above the tape's top, along
// its first 170 design px, in the unit's snapshot.
int tapeOutline(RackStack& stack,RackStackUnit& u)
{
    const auto img=snap(u);const auto k=stack.scale();const auto top=(float)u.getHeight()*0.5f-27.0f*k;
    int white=0;
    for(int x=juce::roundToInt(194.0f*k);x<juce::roundToInt(364.0f*k);++x)
        for(int y=juce::roundToInt(top-9.0f);y<=juce::roundToInt(top-3.0f);++y)white+=luma(img.getPixelAt(x,y))>0.9*255.0?1:0;
    return white;
}
// The AI's marks in the stack (the moves spec: The AI's marks; decisions 7 and 8): a handmade candidate marks Glass
// Current's first control in its row and, folded, round the tape, and outlines Ember Reed's faceplate for its changed
// panel, with the unit's description; the same candidate again draws nothing; none clears them; a rack the stack makes
// while one shows gets it. Glass Current is the house-plate copy, whose row and tape show (its faceplate's marks are
// stack_glass_strip's). With --images: stack-proposal.png and stack-proposal-folded.png.
void stackProposal()
{
    std::cout<<"SECTION stack_proposal\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);auto* press=r.unit(2);
    CHECK(ember!=nullptr&&glass!=nullptr&&press!=nullptr&&ember->face()!=nullptr);
    if(ember==nullptr||glass==nullptr||press==nullptr||ember->face()==nullptr)return;
    // Glass Current's first control turned; Ember Reed kept with one control fewer; Thread Press as it is.
    auto candidate=document(r.p);
    const auto first=firstControl(candidate,1);
    CHECK(first.has_value()&&candidate.personalities!=nullptr);
    if(!first||candidate.personalities==nullptr)return;
    auto& turned=candidate.values[*first];turned=turned<0.5f?turned+0.35f:turned-0.35f;
    auto racks=*candidate.personalities;racks.instances[0].design.panel.controls.pop_back();
    candidate.personalities=std::make_shared<const RackPersonalityState>(std::move(racks));
    r.stack->setProposal(&candidate);pump();
    CHECK(glass->row.marks().size()==1&&glass->proposed()&&ember->face()->proposesPanel()&&ember->proposed()&&press->face()->marks().empty()&&!press->proposed());
    CHECK(ember->getDescription().endsWith("\nAI proposes a new panel")&&!glass->getDescription().contains("AI proposes")&&!press->getDescription().contains("AI proposes"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-proposal.png"),r.body()));
    // Folded, Glass Current's tape has the outline and Thread Press's has none; unfolded, the row's mark is still there.
    glass->fold.onToggle();press->fold.onToggle();pump();
    CHECK(glass->isFolded()&&press->isFolded()&&tapeOutline(*r.stack,*glass)>=40&&tapeOutline(*r.stack,*press)<10);
    std::cout<<"  tape outline: Glass Current "<<tapeOutline(*r.stack,*glass)<<", Thread Press "<<tapeOutline(*r.stack,*press)<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-proposal-folded.png"),r.body()));
    glass->fold.onToggle();press->fold.onToggle();pump();
    CHECK(!glass->isFolded()&&glass->row.marks().size()==1&&ember->face()->proposesPanel());
    // The same candidate again, in a new copy, with the editor's refreshes, draws nothing over the racks.
    const auto copy=candidate;
    CHECK(paintsOver(*glass,[&]{for(int n=0;n<20;++n){r.stack->setProposal(&copy);r.refresh();pump(25);}})==0);
    // None clears the marks, the outline and the description.
    r.stack->setProposal(nullptr);pump();
    CHECK(glass->row.marks().empty()&&!glass->proposed()&&!ember->face()->proposesPanel()&&!ember->proposed()&&!ember->getDescription().contains("AI proposes"));
    // A fourth rack placed, and its first control turned in a candidate the stack has before it makes the rack's unit.
    placeRack(r.p,2,{RackRole::fx,masterChain,2},{{0,5},{1,6}});
    auto later=document(r.p);
    const auto grip=firstControl(later,3);
    CHECK(grip.has_value());
    if(!grip)return;
    auto& moved=later.values[*grip];moved=moved<0.5f?moved+0.35f:moved-0.35f;
    r.stack->setProposal(&later);r.refresh();pump();
    CHECK(r.unit(3)!=nullptr&&r.unit(3)->face()!=nullptr&&r.unit(3)->face()->marks().size()==1&&r.unit(3)->proposed());
}
// Dragging (the moves spec: Dragging; decisions 3, 9, 10 and 11): the 6 px threshold; an FX rack across chains and the
// divider; a sound change ending the drag; a synth rack with its inserts; AI locks; Escape, a release outside the stack
// and a drop at home moving nothing; a release over a control; the drawer closing on pickup; a control's own drag; the
// selection light beside the slot. With --images: stack-drag.png.
void stackDrags()
{
    std::cout<<"SECTION stack_drags\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    if(r.unit(2)==nullptr||r.divider()==nullptr)return;
    const auto all=r.ids();const auto ember=all[0],glass=all[1],press=all[2];
    using Ids=std::vector<juce::Uuid>;
    const auto U=[&](int n){return (float)juce::roundToInt((float)n*r.stack->unitHeight());};
    const Pt grip(150.0f,20.0f);
    const auto in=[&](juce::Uuid id,Pt at){auto* u=r.stack->unit(id);CHECK(u!=nullptr);return u!=nullptr?r.stack->getLocalPoint(u,at):Pt();};
    const auto shows=[&](juce::Uuid id){auto* u=r.stack->unit(id);return u!=nullptr&&u->isVisible();};
    const auto boundControl=[&](juce::Uuid id)->std::optional<std::size_t>{const auto d=document(r.p);if(!d.personalities)return {};const auto& allInstances=d.personalities->instances;const auto at=std::find_if(allInstances.begin(),allInstances.end(),[&](const auto& i){return i.id==id;});return at!=allInstances.end()?firstControl(d,std::size_t(at-allInstances.begin())):std::optional<std::size_t>{};};
    const auto control=[&](juce::Uuid id)->juce::Component*{auto* u=r.stack->unit(id);if(!u)return nullptr;if(auto* face=u->face()){const auto index=boundControl(id);for(const auto& slider:face->face.sliders)if(index&&slider->element.paramIndex==*index&&slider->isShowing())return slider.get();return nullptr;}return find(*u,"rack.focus.0");};
    // The view moved so the line lies 300 px down it, and the pointer's point for it: the line's y plus the grip's 20 px.
    const auto toLine=[&](int y){r.stack->viewport().setViewPosition(0,std::max(0,y-300));pump();return Pt(700.0f,(float)(y+20-r.stack->viewport().getViewPositionY()));};
    const auto lineFor=[&](RackDestination t){for(const auto& [y,d]:r.stack->dropLines())if(d&&*d==t)return y;CHECK(false);return 0;};
    Pointer m{*r.stack};
    // The rack in view, pressed at the grip and moved 12 px up: the drag has started.
    const auto lift=[&](juce::Uuid id)
    {
        if(auto* u=r.stack->unit(id))r.stack->viewport().setViewPosition(0,std::max(0,u->getY()-200));
        pump();const auto at=in(id,grip);m.press(at);m.move(at+Pt(0.0f,-12.0f));
        return r.stack->dragging();
    };
    const auto dragTo=[&](juce::Uuid id,RackDestination t){if(!lift(id))return false;const auto onto=toLine(lineFor(t));m.move(onto);m.release(onto);pump();return !r.stack->dragging();};
    // A press without movement, or with 6 px of it, selects only.
    {
        const auto before=r.revisions();const auto said=r.reported.size();
        if(auto* u=r.stack->unit(glass))r.stack->viewport().setViewPosition(0,std::max(0,u->getY()-200));
        pump();
        const auto at=in(glass,grip);m.press(at);m.move(at+Pt(0.0f,6.0f));
        CHECK(!r.stack->dragging());
        m.release(at+Pt(0.0f,6.0f));
        CHECK(r.stack->selected()==glass&&!r.stack->dragging()&&r.reported.size()==said&&r.revisions()==before);
    }
    // Thread Press, from the shared master onto Synth 1's insert chain: it lifts and hides, the divider stays for the
    // master's line, the slot opens at the line 2U tall, and the drop moves it with its sentence.
    CHECK(lift(press));
    CHECK(r.stack->dragging()&&!r.unit(2)->isVisible()&&r.divider()->isVisible()&&r.stack->dropLines().size()==3&&r.stack->selected()==press);
    {
        const auto y=lineFor({RackRole::fx,0,1});const auto onto=toLine(y);m.move(onto);
        auto* s=r.stack->placement();
        CHECK(s!=nullptr&&std::abs(r.at(*s).getHeight()-U(2))<=1.0f&&std::abs(r.at(*s).getY()-(float)y)<=1.0f);
        m.release(onto);pump();
    }
    CHECK(!r.stack->dragging()&&r.reported[r.reported.size()-1]=="Moved Thread Press to Synth 1 insert, position 2"&&stackIds(r.p)==Ids({ember,press,glass}));
    // The rest builds on this move; without it, its UNDOs would undo the racks' placing.
    if(stackIds(r.p)!=Ids({ember,press,glass}))return;
    // UNDO puts it back.
    CHECK(r.p.undoSoundEdit());r.refresh();pump();
    CHECK(stackIds(r.p)==Ids({ember,glass,press})&&r.unit(2)!=nullptr&&r.unit(2)->isVisible());
    // Onto Synth 1's chain again, then under the divider, which shows for the drag with no master FX rack.
    CHECK(dragTo(press,{RackRole::fx,0,1})&&stackIds(r.p)==Ids({ember,press,glass}));
    CHECK(lift(press)&&r.divider()->isVisible());
    {const auto onto=toLine(lineFor({RackRole::fx,masterChain,0}));m.move(onto);m.release(onto);pump();}
    CHECK(r.reported[r.reported.size()-1]=="Moved Thread Press to Shared Master, position 1"&&stackIds(r.p)==Ids({ember,glass,press}));
    // Review Focus 1: an UNDO during the drag, of the move just made, ends it with no move, and the units show again.
    {
        CHECK(lift(press));
        const auto said=r.reported.size();
        CHECK(r.p.undoSoundEdit());r.refresh();pump();
        bool shown=true;for(auto* u:r.stack->units())shown=shown&&u->isVisible();
        CHECK(!r.stack->dragging()&&shown&&r.stack->dropLines().empty()&&stackIds(r.p)==Ids({ember,press,glass}));
        m.release(m.last);pump();
        CHECK(r.reported.size()==said&&stackIds(r.p)==Ids({ember,press,glass}));
    }
    // A synth rack lifts with its inserts: Ember Reed's 3U with Thread Press's 2U, the slot 5U, dropped at the synth part's
    // foot.
    // The synth order is presentation: no value changes.
    {
        const auto values=document(r.p).values;
        const RackDestination foot{RackRole::synth,masterChain,std::uint8_t(document(r.p).layout.synthCount-1)};
        CHECK(lift(ember));
        CHECK(!shows(ember)&&!shows(press)&&r.stack->dropLines().size()==2);
        const auto onto=toLine(lineFor(foot));m.move(onto);
        auto* s=r.stack->placement();
        CHECK(s!=nullptr&&std::abs(r.at(*s).getHeight()-U(5))<=1.0f);
        m.release(onto);pump();
        CHECK(r.reported[r.reported.size()-1]=="Moved Ember Reed to synth order position "+juce::String(int(foot.position)+1)&&stackIds(r.p)==Ids({glass,ember,press})
              &&document(r.p).values==values);
        std::cout<<"  "<<r.reported[r.reported.size()-1]<<'\n';
    }
    // AI locks do not stop a manual move: Glass Current's bank AI-locked, dragged to the synth part's foot.
    {
        auto d=document(r.p);d.synthLocks[1]=true;CHECK(edit(r.p,d));r.refresh();pump();
        CHECK(dragTo(glass,{RackRole::synth,masterChain,std::uint8_t(d.layout.synthCount-1)}));
        CHECK(r.reported[r.reported.size()-1].startsWith("Moved Glass Current to synth order position")&&stackIds(r.p)==Ids({ember,press,glass})&&document(r.p).synthLocks[1]);
    }
    // Escape, a release outside the stack, and a drop where it started: nothing moves and nothing is said.
    {
        const auto said=r.reported.size();const auto before=r.revisions();
        CHECK(lift(press));m.escape();
        CHECK(!r.stack->dragging()&&shows(press));
        m.release(m.last);
        CHECK(lift(press));m.move(toLine(lineFor({RackRole::fx,masterChain,0})));m.release(Pt(700.0f,-60.0f));pump();
        CHECK(!r.stack->dragging()&&shows(press));
        CHECK(lift(press));m.release(m.last);pump();
        CHECK(!r.stack->dragging()&&r.reported.size()==said&&r.revisions()==before&&stackIds(r.p)==Ids({ember,press,glass}));
    }
    // Review Focus 5: a release over a control lands on the slot's line.
    {
        CHECK(lift(press));
        m.move(toLine(lineFor({RackRole::fx,1,2})));
        auto* knob=control(glass);
        CHECK(knob!=nullptr);
        if(knob!=nullptr)
        {
            const auto over=r.stack->getLocalPoint(knob,knob->getLocalBounds().getCentre().toFloat());
            CHECK(r.stack->getLocalBounds().toFloat().contains(over));
            m.release(over);pump();
        }
        CHECK(r.reported[r.reported.size()-1]=="Moved Thread Press to Synth 2 insert, position 3"&&stackIds(r.p)==Ids({ember,glass,press}));
    }
    // The drawer closes on pickup; a press on a control turns it and starts no drag.
    {
        r.stack->openDrawer(ember);pump();
        CHECK(r.stack->drawerRack()==ember);
        CHECK(lift(ember)&&r.stack->drawerRack().isNull());
        m.escape();m.release(m.last);
        r.stack->viewport().setViewPosition(0,0);pump();
        auto* knob=control(ember);
        const auto air=boundControl(ember);
        CHECK(knob!=nullptr&&knob->isShowing()&&air.has_value());
        if(knob!=nullptr&&air)
        {
            const auto was=r.p.control(*air).getValue();
            const auto at=r.stack->getLocalPoint(knob,knob->getLocalBounds().getCentre().toFloat());
            auto* hit=r.stack->getComponentAt(at.roundToInt());
            std::cout<<"STACK_CONTROL_DIAGNOSTIC showing="<<knob->isShowing()<<" id="<<knob->getComponentID()<<" title="<<knob->getTitle()<<" index="<<*air<<" parameter="<<parameterRegistry()[*air].stateKey<<" legacyFirst="<<firstControl(document(r.p),0).value_or(parameterCount)<<" was="<<was<<" hit="<<(hit?hit->getComponentID():juce::String("none"))<<std::endl;
            m.press(at);m.move(at+Pt(0.0f,-20.0f));m.move(at+Pt(0.0f,-40.0f));
            CHECK(!r.stack->dragging());
            m.release(at+Pt(0.0f,-40.0f));
            CHECK(r.p.control(*air).getValue()!=was);
        }
    }
    // The selection light runs beside the slot while dragging, and not beside the rack's old place: Ember Reed, alone on
    // its chain, held at the synth part's foot, its old place under Glass Current.
    {
        auto* e=r.stack->unit(ember);
        CHECK(e!=nullptr);
        if(e==nullptr)return;
        const auto old=r.at(*e).getCentreY();
        CHECK(lift(ember)&&r.stack->dropLines().size()==2);
        m.move(toLine(lineFor({RackRole::synth,masterChain,std::uint8_t(document(r.p).layout.synthCount-1)})));
        auto* s=r.stack->placement();
        CHECK(s!=nullptr);
        if(s!=nullptr)
        {
            const auto img=r.body();const auto right=((float)studio::HouseUnit::railW+r.model.house.panelWidth)*r.stack->scale();
            const auto lit=img.getPixelAt((int)(right+1.5f),(int)r.at(*s).getCentreY()),dark=img.getPixelAt((int)(right+1.5f),(int)old);
            CHECK(lit.getRed()>200&&lit.getBlue()>230&&dark.getRed()<100);
            std::cout<<"  beside the slot "<<lit.toDisplayString(false)<<", beside the old place "<<dark.toDisplayString(false)<<'\n';
        }
        m.escape();m.release(m.last);
    }
    // Thread Press held over the divider, with the slot on the shared master.
    if(output!=juce::File{})
    {
        CHECK(lift(press));
        m.move(toLine(lineFor({RackRole::fx,masterChain,0}))+Pt(0.0f,-60.0f));
        CHECK(writePng(output.getChildFile("stack-drag.png"),r.body()));
        m.escape();m.release(m.last);
    }
    CHECK(!r.stack->dragging());
}
// The edge scroll (decision 10): Ember Reed held 10 px from the view's bottom scrolls the view down, and 10 px from its
// top scrolls it up, the slot at the line nearest the copy each time; Escape ends the drag.
void stackDragScrolls()
{
    std::cout<<"SECTION stack_drag_scrolls\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);
    CHECK(ember!=nullptr);
    if(ember==nullptr)return;
    const auto h=(float)r.stack->viewport().getHeight();
    // The line nearest the copy's top: the pointer's y in the body less the grip's 20 px.
    const auto nearest=[&](float pointer)
    {
        const auto copy=(float)r.stack->viewport().getViewPositionY()+pointer-20.0f;
        int best=-1;float gap=1e9f;
        for(const auto& [y,d]:r.stack->dropLines())if(std::abs((float)y-copy)<gap){gap=std::abs((float)y-copy);best=y;}
        return (float)best;
    };
    const auto slotAt=[&](float pointer){auto* s=r.stack->placement();return s!=nullptr&&std::abs(r.at(*s).getY()-nearest(pointer))<=1.0f;};
    Pointer m{*r.stack};
    r.stack->viewport().setViewPosition(0,0);pump();
    const auto at=r.stack->getLocalPoint(ember,Pt(150.0f,20.0f));
    m.press(at);m.move(at+Pt(0.0f,12.0f));
    CHECK(r.stack->dragging());
    m.move(Pt(700.0f,h-10.0f));pump(300);
    const auto down=r.stack->viewport().getViewPositionY();
    CHECK(down>0&&slotAt(h-10.0f));
    m.move(Pt(700.0f,10.0f));pump(300);
    CHECK(r.stack->viewport().getViewPositionY()<down&&slotAt(10.0f));
    std::cout<<"  view: "<<down<<" at the foot, "<<r.stack->viewport().getViewPositionY()<<" at the top\n";
    m.escape();m.release(m.last);
    CHECK(!r.stack->dragging()&&ember->isVisible());
}
// Whether a label in c that shows is lettered `text`, as the identity block's are.
bool lettered(juce::Component& c,const juce::String& text)
{
    for(auto* child:c.getChildren())
    {
        if(auto* l=dynamic_cast<juce::Label*>(child);l!=nullptr&&l->isShowing()&&l->getText()==text)return true;
        if(lettered(*child,text))return true;
    }
    return false;
}
// The faceplate in the stack (the maker faceplates spec: Which racks get a faceplate, The host's parts on a faceplate,
// Keyboard and accessibility): THREAD PRESS shows MARLOW & FINCH's 2U faceplate between the rails, bound to its own
// banks, instead of its identity block, UP, DOWN and row; a copy of Glass Current under another design ID keeps the house
// plate (Ember Reed's faceplate is stack_ember's, Glass Current's stack_glass's); the same sound again draws nothing over
// it.
void stackFaceplate()
{
    std::cout<<"SECTION stack_faceplate\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);auto* press=r.unit(2);
    CHECK(ember!=nullptr&&glass!=nullptr&&press!=nullptr);
    if(ember==nullptr||glass==nullptr||press==nullptr)return;
    auto* face=press->face();
    CHECK(face!=nullptr&&ember->face()!=nullptr&&glass->face()==nullptr&&find(*glass,"rack.face")==nullptr);
    if(face==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();
    CHECK(find(*press,"rack.face")==face&&face->isShowing()&&!face->isOpen()&&press->units()==2&&std::abs(r.at(*press).getHeight()-2.0f*u)<=1.0f);
    CHECK(closeTo(r.at(*face),Rect((float)studio::HouseUnit::railW*k,r.at(*press).getY(),2140.0f*k,394.0f*k)));
    CHECK(!lettered(*press,"THREAD PRESS")&&!lettered(*press,"SHARED MASTER")&&!press->up.isShowing()&&!press->down.isShowing()&&!press->row.isShowing()
          &&press->operations.isShowing());
    CHECK(lettered(*glass,"GLASS CURRENT")&&glass->row.isShowing()&&glass->up.isShowing());
    CHECK(press->name()=="THREAD PRESS"&&press->place()=="SHARED MASTER"&&press->getTitle()=="Thread Press");
    const auto d=document(r.p);
    CHECK(press->getDescription()==rackHeaderText(d,d.personalities->instances[2])+"\nMARLOW & FINCH, PROGRAMME COMPRESSOR TYPE TP-2");
    // Its own banks: the compressor in effect bank 4, the drive in bank 5.
    auto* grip=face->face.slider<studio::house::Knob>("grip");auto* density=face->face.slider<studio::house::Knob>("density");auto* in=face->face.lever("in");
    CHECK(grip!=nullptr&&density!=nullptr&&in!=nullptr);
    if(grip!=nullptr&&density!=nullptr&&in!=nullptr)
        CHECK(grip->element.paramIndex==idx("effect.4.compressor.threshold_db")&&density->element.paramIndex==idx("effect.5.drive.gain_db")
              &&in->element.paramIndex==idx("effect.4.bypassed"));
    CHECK(paintsOver(*press,[&]{for(int n=0;n<20;++n){r.refresh();pump(25);}})==0);
}
// The narrow-width rule's rack unit: the column folds it as view state, its own fold kept; its plate's parts lie in a
// pane that clips at the right rail, shifts them sideways and takes no press under the left rail; a sideways wheel off
// its controls reports a distance; a press on its triangle while the column folds it chooses it and unfolds it.
void stackUnitNarrow()
{
    std::cout<<"SECTION stack_unit_narrow\n";
    StackRig r;
    drawnRacks(r.p,true);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);
    CHECK(ember!=nullptr&&glass!=nullptr&&ember->face()!=nullptr&&glass->face()==nullptr);
    if(ember==nullptr||glass==nullptr||ember->face()==nullptr)return;
    const auto units=ember->units();
    ember->setColumnFolded(true);pump();
    CHECK(ember->isFolded()&&ember->columnFolded()&&!ember->foldedByUser()&&ember->units()==1&&ember->fold.folded&&ember->fold.getTitle()=="Unfold Ember Reed");
    ember->setColumnFolded(false);pump();
    CHECK(!ember->isFolded()&&ember->units()==units&&!ember->fold.folded&&ember->fold.getTitle()=="Fold Ember Reed");
    // The triangle while the column folds it: the stack chooses it, and the user's fold, if any, opens. At the stage's
    // width the stack's layout after the press takes the column's fold away again.
    int chosen=0;
    ember->onChosen=[&]{++chosen;};
    ember->setColumnFolded(true);pump();
    pressThrough(ember->fold,{(float)ember->fold.getWidth()*.5f,34.0f});
    CHECK(chosen==1&&!ember->foldedByUser());
    ember->setColumnFolded(true);ember->setFolded(true);pump();
    pressThrough(ember->fold,{(float)ember->fold.getWidth()*.5f,34.0f});
    CHECK(chosen==2&&!ember->foldedByUser());
    ember->setColumnFolded(false);pump();
    CHECK(!ember->isFolded());
    // In a column 1143 px wide the pane ends at the right rail, 1112; the faceplate shifts within 1516 - 1112 px.
    ember->setSize(1143,ember->getHeight());
    auto* pane=find(*ember,"rack.pane");
    CHECK(pane!=nullptr&&ember->across()==404&&ember->shift()==0);
    if(pane==nullptr)return;
    CHECK(pane->getBounds()==juce::Rectangle<int>(0,0,1112,ember->getHeight())&&ember->plate().getParentComponent()==pane);
    const auto faceX=r.at(*ember->face()).getX();
    ember->setShift(200);pump();
    CHECK(ember->shift()==200&&std::abs(r.at(*ember->face()).getX()-(faceX-200.0f))<0.01f&&std::abs(r.at(ember->operations).getX()-(r.at(*ember).getX()+1116.0f))<0.01f);
    ember->setShift(9999);
    CHECK(ember->shift()==404);
    ember->setShift(200);
    auto* under=ember->getComponentAt(20,ember->getHeight()/2);
    CHECK(under!=nullptr&&under!=ember->face()&&!ember->face()->isParentOf(under));
    // Folded, nothing lies across.
    ember->setColumnFolded(true);
    CHECK(ember->across()==0&&ember->shift()==0);
    ember->setColumnFolded(false);
    // A house plate shifts its row, keys and OPERATIONS.
    glass->setSize(1143,glass->getHeight());
    const auto rowX=r.at(glass->row).getX(),upX=r.at(glass->up).getX(),opX=r.at(glass->operations).getX();
    glass->setShift(200);
    CHECK(glass->shift()==200&&std::abs(r.at(glass->row).getX()-(rowX-200.0f))<0.01f&&std::abs(r.at(glass->up).getX()-(upX-200.0f))<0.01f
          &&std::abs(r.at(glass->operations).getX()-(opX-200.0f))<0.01f);
    // A horizontal wheel, or Shift and the wheel, off the controls reports a distance; a vertical wheel scrolls the rack
    // instead, and once that scroll has settled a wheel over AIR turns AIR.
    int across=0;
    ember->onAcross=[&](int px){across+=px;};
    auto& face=*ember->face();
    juce::MouseWheelDetails w;
    w.deltaX=-0.25f;w.deltaY=0.0f;w.isReversed=w.isSmooth=w.isInertial=false;
    face.mouseWheelMove(event(face,{60.0f,300.0f},{},0,false),w);
    CHECK(across==56);
    w.deltaX=0.0f;w.deltaY=-0.25f;
    face.mouseWheelMove(event(face,{60.0f,300.0f},juce::ModifierKeys::shiftModifier,0,false),w);
    CHECK(across==112);
    face.mouseWheelMove(event(face,{60.0f,300.0f},{},0,false),w);
    CHECK(across==112);
    if(auto* air=face.face.slider<studio::house::Knob>("air"))
    {
        juce::Thread::sleep(450);
        const auto before=air->getValue();
        spin(*air,air->getLocalBounds().toFloat().getCentre(),0.25f);
        CHECK(air->getValue()!=before&&across==112);
    }
    else CHECK(!"no AIR");
}
// Whether a pixel is within 6 of a colour in each channel.
bool near6(juce::Colour a,juce::Colour b)
{
    return std::abs(a.getRed()-b.getRed())<=6&&std::abs(a.getGreen()-b.getGreen())<=6&&std::abs(a.getBlue()-b.getBlue())<=6;
}
// The narrow stack (the narrow-width rule spec, decisions 2 to 4 and 8): narrower than the stage's 1547 px, every rack
// but the selected one shows its strip as view state; selecting, the focus or a press on a folded triangle opens a rack,
// the press keeping its top in the view; the house strip, divider, drawer, space and a placement fit the column; at the
// stage's width the units show their own folds. With --images: stack-narrow.png.
void stackNarrow()
{
    std::cout<<"SECTION stack_narrow\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);auto* press=r.unit(2);auto* divider=r.divider();
    CHECK(ember!=nullptr&&glass!=nullptr&&press!=nullptr&&divider!=nullptr);
    if(ember==nullptr||glass==nullptr||press==nullptr||divider==nullptr)return;
    const auto ids=r.ids();auto& view=r.stack->viewport();
    CHECK(!r.stack->narrow());
    r.stack->setSize(1143,754);pump();
    CHECK(r.stack->narrow()&&r.stack->body().getWidth()==1143&&r.stack->selected()==ids[0]);
    // The right rail from the column's edge, 1112: its edge, then its fill, beside the divider's plate.
    {
        const auto img=r.body();const auto y=(int)r.at(*divider).getCentreY();
        CHECK(near6(img.getPixelAt(1114,y),juce::Colour(0xff141517))&&!near6(img.getPixelAt(1110,y),juce::Colour(0xff141517)));
        if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-narrow.png"),img));
    }
    // The divider across the column, its tape centred.
    {
        const auto img=snap(*divider);int lo=img.getWidth(),hi=-1;
        for(int x=120;x<img.getWidth()-120;++x)
            for(int y=img.getHeight()/2-10;y<=img.getHeight()/2+10;++y)
                if(luma(img.getPixelAt(x,y))>150.0){lo=std::min(lo,x);hi=std::max(hi,x);}
        CHECK(divider->getWidth()==1143&&hi>lo&&std::abs((float)(lo+hi+1)*0.5f-571.5f)<=1.0f);
        std::cout<<"  divider tape: "<<lo<<" to "<<hi<<'\n';
    }
    CHECK(std::abs(r.at(*r.house).getWidth()-1112.0f)<=1.0f&&r.house->narrow.isVisible()&&!r.house->unit.isVisible()&&r.house->narrowWidth()==1557);
    const auto folds=[&](RackStackUnit* open)
    {
        bool ok=true;
        for(auto* u:r.stack->units())
            ok=ok&&!u->foldedByUser()&&(u==open?!u->isFolded()&&!u->columnFolded():u->columnFolded()&&u->units()==1);
        return ok;
    };
    CHECK(folds(ember)&&ember->units()==3);
    r.stack->select(ids[1]);pump();
    CHECK(folds(glass)&&glass->units()==2);
    pressRail(glass->fold);pump();
    CHECK(glass->foldedByUser()&&glass->isFolded()&&r.stack->selected()==ids[1]);
    pressRail(glass->fold);pump();
    CHECK(!glass->isFolded()&&folds(glass));
    press->fold.grabKeyboardFocus();pump();
    CHECK(r.stack->selected()==ids[2]&&folds(press));
    // Ember Reed open, the view at its foot: a press on Thread Press's triangle opens it, its top where it was.
    r.stack->select(ids[0]);pump();
    view.setViewPosition(0,r.stack->body().getHeight());pump();
    const auto top=r.at(*press).getY()-(float)view.getViewPositionY();
    pressThrough(press->fold,{22.0f,30.0f});
    CHECK(r.stack->selected()==ids[2]&&folds(press)&&std::abs(r.at(*press).getY()-(float)view.getViewPositionY()-top)<=1.0f);
    // The drawer between the rails' inner edges; the space and its + ADD RACK within the column.
    r.stack->openDrawer(ids[0]);pump();
    CHECK(r.stack->drawerRack()==ids[0]&&folds(ember)&&r.at(r.drawer).getX()==31.0f&&r.at(r.drawer).getRight()==1112.0f);
    r.stack->closeDrawer();pump();
    CHECK(r.at(r.space).getWidth()==1112.0f&&r.space.add.getRight()<=1112);
    // A placement's ghost to the right rail's inner edge.
    auto design=airack::test::frozenExampleRackDesigns()[1];design.id=juce::Uuid();
    r.stack->setPlacement(StackPlacement{{RackRole::synth,masterChain,2},"PREVIEW: SYNTH 3, AFTER GLASS CURRENT",rackPreviewInstance(design),rackPreviewValues(design)});pump();
    auto* s=r.stack->placement();
    CHECK(s!=nullptr&&s->getNumChildComponents()==1&&s->getChildComponent(0)->getWidth()==1112);
    r.stack->setPlacement(std::nullopt);pump();
    // The window losing the focus and getting it back, on Thread Press's triangle, leaves Glass Current selected and open.
    r.stack->select(ids[1]);pump();
    CHECK(press->fold.hasKeyboardFocus(false));
    auto* peer=r.holder.getPeer();
    CHECK(peer!=nullptr);
    if(peer!=nullptr){peer->handleFocusLoss();pump();peer->handleFocusGain();pump();}
    CHECK(r.stack->selected()==ids[1]&&folds(glass)&&press->fold.hasKeyboardFocus(false));
    // At the stage's width each unit shows its own fold, Glass Current folded by the user while it was open.
    pressRail(glass->fold);pump();
    CHECK(glass->foldedByUser()&&glass->isFolded());
    r.stack->setSize(1547,754);pump();
    bool own=true;for(auto* u:r.stack->units())own=own&&!u->columnFolded();
    CHECK(!r.stack->narrow()&&own&&glass->isFolded()&&!ember->isFolded()&&!press->isFolded()&&r.house->narrowWidth()==0&&r.house->unit.isVisible());
}
// The open unit's sideways scroll (the narrow-width rule spec, decision 5): rack.sideways along its foot between the
// rails with the plate's range; the bar, a sideways wheel and the focus scroll it; a door opened at the right end
// scrolls into view; the bar is titled with the unit's name and takes no focus.
void stackNarrowScroll()
{
    std::cout<<"SECTION stack_narrow_scroll\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);
    CHECK(ember!=nullptr&&glass!=nullptr&&ember->face()!=nullptr&&glass->face()!=nullptr);
    if(ember==nullptr||glass==nullptr||ember->face()==nullptr||glass->face()==nullptr)return;
    const auto ids=r.ids();auto& bar=r.stack->sideways();
    CHECK(!bar.isVisible());
    r.stack->setSize(1143,754);pump();
    const auto foot=r.at(*ember).getBottom();
    CHECK(bar.isVisible()&&bar.getComponentID()=="rack.sideways"&&r.stack->body().isParentOf(&bar)&&closeTo(r.at(bar),Rect(39.0f,foot-12.0f,1065.0f,8.0f))
          &&bar.getMaximumRangeLimit()==1485.0&&bar.getCurrentRangeSize()==1081.0&&bar.getCurrentRangeStart()==0.0&&r.stack->openUnit()==ember);
    CHECK(bar.getTitle()=="Scroll EMBER REED sideways"&&!bar.getWantsKeyboardFocus()&&bar.getAccessibilityHandler()!=nullptr
          &&bar.getAccessibilityHandler()->getRole()==juce::AccessibilityRole::scrollBar);
    bar.setCurrentRangeStart(200.0,juce::sendNotificationSync);pump();
    CHECK(ember->shift()==200);
    ember->onAcross(-50);pump();
    CHECK(ember->shift()==150&&bar.getCurrentRangeStart()==150.0);
    auto* grain=ember->face()->face.slider<studio::house::Knob>("grain");
    CHECK(grain!=nullptr);
    if(grain!=nullptr)
    {
        grain->grabKeyboardFocus();pump();
        CHECK(std::abs(r.at(*grain).getRight()-1112.0f)<=1.0f&&bar.getCurrentRangeStart()==(double)ember->shift());
        std::cout<<"  GRAIN at "<<r.at(*grain).toString()<<", shift "<<ember->shift()<<'\n';
    }
    // Glass Current open and scrolled to its right end: its door, opened from the keyboard, comes into view.
    r.stack->select(ids[1]);pump();
    CHECK(r.stack->openUnit()==glass&&glass->shift()==0&&bar.getTitle()=="Scroll GLASS CURRENT sideways");
    glass->setShift(9999);
    auto* face=glass->face();auto* edit=face->face.button("edit");
    CHECK(edit!=nullptr);
    if(edit==nullptr)return;
    edit->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    const auto door=r.stack->body().getLocalArea(face,face->openedArea().toFloat());
    CHECK(face->isOpen()&&!door.isEmpty()&&(door.getWidth()>1081.0f?std::abs(door.getX()-31.0f)<=1.0f:door.getX()>=30.5f&&door.getRight()<=1112.5f)
          &&std::abs(r.at(bar).getBottom()-(r.at(*glass).getBottom()-4.0f))<=1.0f);
    std::cout<<"  door "<<door.toString()<<", shift "<<glass->shift()<<'\n';
}
// The house unit opened in a narrow column (the narrow-width rule spec, decision 7): its 3U unit scrolls sideways and
// every rack folds, the selected one keeping its selection; its triangle, or selecting a rack, folds it back.
void stackNarrowHouse()
{
    std::cout<<"SECTION stack_narrow_house\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    const auto ids=r.ids();auto& bar=r.stack->sideways();
    CHECK(ids.size()==3);
    if(ids.size()!=3)return;
    r.stack->openHouse(true);pump();
    CHECK(!r.stack->houseOpen()&&!r.house->isOpen());
    r.stack->setSize(1143,754);pump();
    r.stack->openHouse(true);pump();
    bool folded=true;for(auto* u:r.stack->units())folded=folded&&u->columnFolded()&&u->units()==1;
    const auto house=r.at(*r.house);
    CHECK(r.stack->houseOpen()&&r.house->unit.isVisible()&&!r.house->showsStrip()&&folded&&r.stack->selected()==ids[0]&&r.stack->openUnit()==r.house.get());
    CHECK(bar.isVisible()&&bar.getMaximumRangeLimit()==2140.0&&bar.getCurrentRangeSize()==1557.0&&std::abs(r.at(bar).getBottom()-(house.getBottom()-4.0f))<=1.0f
          &&bar.getTitle()=="Scroll AI RACK SYNTH sideways");
    r.house->onAcross(100);pump();
    CHECK(r.house->shift()==100&&bar.getCurrentRangeStart()==100.0);
    bar.setCurrentRangeStart(300.0,juce::sendNotificationSync);pump();
    CHECK(r.house->shift()==300);
    pressRail(r.house->rail);pump();
    CHECK(!r.stack->houseOpen()&&r.house->showsStrip()&&r.house->narrow.isVisible()&&!r.stack->unit(ids[0])->isFolded());
    r.stack->openHouse(true);pump();
    CHECK(r.stack->houseOpen()&&r.house->shift()==0);
    r.stack->select(ids[0]);pump();
    CHECK(!r.stack->houseOpen()&&r.stack->selected()==ids[0]&&!r.stack->unit(ids[0])->isFolded());
    // At the stage's width the house shows its own fold.
    r.stack->openHouse(true);pump();
    r.stack->setSize(1547,754);pump();
    CHECK(!r.stack->houseOpen()&&r.house->narrowWidth()==0&&r.house->unit.isVisible()&&!r.house->isFolded()&&!bar.isVisible());
}
// Review Focus 1: resizing while narrow keeps the open unit's scroll within range, and the house strip spans the column.
void stackNarrowResize()
{
    std::cout<<"SECTION stack_narrow_resize\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);
    CHECK(ember!=nullptr);
    if(ember==nullptr)return;
    r.stack->setSize(1143,754);pump();
    ember->setShift(9999);pump();
    CHECK(ember->shift()==404);
    r.stack->setSize(1300,754);pump();
    const auto column=(int)std::floor(1269.0f/r.stack->scale())-studio::HouseUnit::railW;
    CHECK(r.stack->narrow()&&ember->across()==247&&ember->shift()==247&&r.stack->sideways().getCurrentRangeStart()==247.0
          &&r.house->narrowWidth()==column&&std::abs(r.at(*r.house).getWidth()-1269.0f)<=1.0f);
    r.stack->setSize(1143,754);pump();
    CHECK(ember->shift()==247&&r.house->narrowWidth()==1557);
}
// Review Focus 2: a drag that starts on a column-folded rack opens it on the press and moves it when dropped.
void stackNarrowDrag()
{
    std::cout<<"SECTION stack_narrow_drag\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* press=r.unit(2);
    CHECK(press!=nullptr);
    if(press==nullptr)return;
    const auto all=r.ids();const auto ember=all[0],glass=all[1],thread=all[2];
    r.stack->setSize(1143,754);pump();
    CHECK(press->columnFolded());
    auto& view=r.stack->viewport();
    view.setViewPosition(0,std::max(0,press->getY()-100));pump();
    Pointer m{*r.stack};
    const auto at=r.stack->getLocalPoint(press,Pt(150.0f,20.0f));
    m.press(at);
    CHECK(r.stack->selected()==thread&&!press->isFolded());
    m.move(at+Pt(0.0f,-12.0f));
    CHECK(r.stack->dragging()&&!press->isVisible());
    int y=0;for(const auto& [line,d]:r.stack->dropLines())if(d&&*d==RackDestination{RackRole::fx,0,1})y=line;
    view.setViewPosition(0,std::max(0,y-300));pump();
    const Pt onto(700.0f,(float)(y+20-view.getViewPositionY()));
    m.move(onto);m.release(onto);pump();
    CHECK(!r.stack->dragging()&&!r.reported.isEmpty()&&r.reported[r.reported.size()-1]=="Moved Thread Press to Synth 1 insert, position 2"
          &&stackIds(r.p)==std::vector<juce::Uuid>({ember,thread,glass}));
}
// Review Focus 3: the AI's marks while narrow: a column-folded faceplate's strip outlines its display, and the open
// faceplate's marks scroll with it.
void stackNarrowMarks()
{
    std::cout<<"SECTION stack_narrow_marks\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);
    auto* face=glass!=nullptr?glass->face():nullptr;
    const studio::house::Element* name=nullptr;
    if(face!=nullptr)for(const auto& e:face->face.ctx.model.elements)if(e.id=="strip.name")name=&e;
    auto* air=ember!=nullptr&&ember->face()!=nullptr?ember->face()->face.slider<studio::house::Knob>("air"):nullptr;
    CHECK(ember!=nullptr&&face!=nullptr&&name!=nullptr&&air!=nullptr);
    if(ember==nullptr||face==nullptr||name==nullptr||air==nullptr)return;
    r.stack->setSize(1143,754);pump();
    CHECK(glass->columnFolded()&&face->isFolded());
    const auto k=r.stack->scale(),railX=(float)studio::HouseUnit::railW*k;
    const auto ring=[&]
    {
        const auto img=snap(*glass);const auto top=(name->rect*k).translated(railX,0.0f).expanded(3.0f);int n=0;
        for(int x=juce::roundToInt(top.getX());x<std::min(1112,juce::roundToInt(top.getRight()));++x)
            for(int y=juce::roundToInt(top.getY())-2;y<=juce::roundToInt(top.getY())+2;++y)n+=luma(img.getPixelAt(x,y))>0.9*255.0?1:0;
        return n;
    };
    const auto plain=ring();
    const auto fa=idx("synth.2.wavetable.osc.1.position"),cutoff=idx("synth.1.analog.filter.cutoff");
    auto candidate=document(r.p);candidate.values[fa]=candidate.values[fa]<0.5f?candidate.values[fa]+0.35f:candidate.values[fa]-0.35f;
    candidate.values[cutoff]=r.p.control(cutoff).convertTo0to1(2000.0f);
    r.stack->setProposal(&candidate);pump();
    CHECK(glass->proposed()&&ember->face()->marks().size()==1&&ring()>plain+40);
    std::cout<<"  display outline: "<<plain<<" then "<<ring()<<'\n';
    // AIR and its proposed pointer, 100 px further left once shifted.
    const auto box=r.stack->body().getLocalArea(air,air->getLocalBounds().toFloat());
    const auto before=r.body();
    ember->setShift(100);pump();
    const auto after=r.body();
    CHECK(box.getX()-100.0f>=31.0f&&std::abs(meanLuma(before,box)-meanLuma(after,box.translated(-100.0f,0.0f)))<2.0);
    r.stack->setProposal(nullptr);pump();
}
// Review Focus 5: the open rack retired while scrolled: the next selected rack opens at its left end and the bar follows.
void stackNarrowRetire()
{
    std::cout<<"SECTION stack_narrow_retire\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);
    CHECK(ember!=nullptr);
    if(ember==nullptr)return;
    const auto ids=r.ids();
    r.stack->setSize(1143,754);pump();
    r.stack->sideways().setCurrentRangeStart(300.0,juce::sendNotificationSync);pump();
    CHECK(ember->shift()==300&&r.stack->sideways().getCurrentRangeStart()==300.0);
    RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=ids[0];
    SoundDocument now;std::uint64_t revision=0;
    CHECK(r.p.trySoundSnapshot(now,revision)&&r.p.applyRackOperation(remove,revision)==RackCandidateStatus::ready);
    r.refresh();pump();
    auto* glass=r.stack->unit(ids[1]);
    CHECK(glass!=nullptr&&r.stack->selected()==ids[1]&&r.stack->openUnit()==glass);
    if(glass==nullptr)return;
    auto& bar=r.stack->sideways();
    CHECK(glass->shift()==0&&!glass->isFolded()&&bar.isVisible()&&bar.getCurrentRangeStart()==0.0&&bar.getTitle()=="Scroll GLASS CURRENT sideways"
          &&std::abs(r.at(bar).getBottom()-(r.at(*glass).getBottom()-4.0f))<=1.0f);
}
// The swap (the maker faceplates spec: Which racks get a faceplate): an edit of what the faceplate draws, a focused
// control's label, puts Thread Press on the house plate with its row, and the focus inside the faceplate goes to the
// triangle; the undo brings the faceplate back.
void stackFaceplateSwaps()
{
    std::cout<<"SECTION stack_faceplate_swaps\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* press=r.unit(2);
    CHECK(press!=nullptr&&press->face()!=nullptr);
    if(press==nullptr||press->face()==nullptr)return;
    auto* grip=press->face()->face.slider<studio::house::Knob>("grip");
    CHECK(grip!=nullptr);
    if(grip==nullptr)return;
    grip->grabKeyboardFocus();pump();
    CHECK(grip->hasKeyboardFocus(false));
    // The drawer's remap: Thread Press moved to its own place with GRIP's control relabelled, one undoable edit.
    const auto d=document(r.p);const auto& i=*std::find_if(d.personalities->instances.begin(),d.personalities->instances.end(),[&](const auto& x){return x.id==press->id();});
    auto hold=i.design.panel.controls[0];hold.label="Hold";
    RackOperation op;op.kind=RackOperationKind::move;op.selected=i.id;op.destination={RackRole::fx,masterChain,0};op.choices.controls.push_back({i.id,0,hold});
    SoundDocument now;std::uint64_t revision=0;
    CHECK(r.p.trySoundSnapshot(now,revision)&&r.p.applyRackOperation(op,revision)==RackCandidateStatus::ready);r.refresh();pump();
    CHECK(press->face()==nullptr&&find(*press,"rack.face")==nullptr&&press->row.isShowing()&&lettered(*press,"THREAD PRESS")&&press->units()==2
          &&!press->getDescription().contains("MARLOW")&&juce::Component::getCurrentlyFocusedComponent()==&press->fold);
    CHECK(r.p.undoSoundEdit());
    CHECK(wait("the faceplate back",[&]{r.refresh();return press->face()!=nullptr;})&&!press->row.isShowing()&&!lettered(*press,"THREAD PRESS"));
}
// OPERATIONS on the faceplate's right rail (the maker faceplates spec, decision 5): above the unit's top rail number; it
// opens the drawer under Thread Press and selects it, lit, and closes it again; Tab runs the triangle, the faceplate's
// controls, the open recess's after SET-UP, then OPERATIONS.
void stackFaceplateOperations()
{
    std::cout<<"SECTION stack_faceplate_operations\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* press=r.unit(2);
    CHECK(press!=nullptr&&press->face()!=nullptr);
    if(press==nullptr||press->face()==nullptr)return;
    const auto right=((float)studio::HouseUnit::railW+r.model.house.panelWidth)*r.stack->scale();
    const auto key=r.at(press->operations),unit=r.at(*press);
    const auto number=unit.getY()+0.5f*r.stack->unitHeight();
    CHECK(key.getX()>=right&&key.getRight()<=1547.0f&&key.getY()>=unit.getY()&&key.getBottom()<=number-12.0f);
    std::cout<<"  OPERATIONS at "<<key.toString()<<", the rail number at "<<number<<'\n';
    CHECK(press->operations.getComponentID()=="rack.operations"&&press->operations.getTitle()=="Operations on Thread Press");
    const auto lens=[&]{return meanLuma(snap(press->operations),Rect(5.0f,5.0f).withCentre(press->operations.getLocalBounds().toFloat().getCentre()));};
    const auto unlit=lens();
    click(press->operations);
    CHECK(r.stack->drawerRack()==press->id()&&r.stack->selected()==press->id()&&press->operations.getToggleState()&&r.drawerUnder(*press));
    const auto lit=lens();
    CHECK(lit>unlit+40.0);
    std::cout<<"  OPERATIONS lens: "<<lit<<" lit, "<<unlit<<" unlit\n";
    click(press->operations);
    CHECK(r.stack->drawerRack().isNull()&&!press->operations.getToggleState()&&!r.drawer.isVisible());
    const auto order=[&]
    {
        juce::StringArray got;
        for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(press->isParentOf(c))got.add(c->getComponentID());
        return got;
    };
    juce::StringArray closed{"rack.fold","face.grip","face.front","face.blend","face.density","face.in","face.setup"};
    auto open=closed;open.addArray(juce::StringArray{"face.ratio","face.release","face.makeup","face.shape","face.tone","face.mix","face.drive-in","face.close"});
    closed.add("rack.operations");open.add("rack.operations");
    CHECK(order()==closed);
    if(order()!=closed)std::cout<<"  Tab order: "<<order().joinIntoString(" ")<<'\n';
    press->face()->setOpen(true);pump();
    CHECK(order()==open);
    if(order()!=open)std::cout<<"  open Tab order: "<<order().joinIntoString(" ")<<'\n';
}
// Presses on the faceplate (the maker faceplates spec: The host's parts on a faceplate; decision 7): GRIP turns, IN / OUT
// flips and SET-UP opens, none starting a drag; a press on the brass plate or the left ear selects Thread Press, leaving
// the focus where it was, as a press on a house plate does, and a 20 px drag lifts it.
void stackFaceplatePresses()
{
    std::cout<<"SECTION stack_faceplate_presses\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* press=r.unit(2);auto* face=press!=nullptr?press->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    const auto ids=r.ids();
    auto* grip=face->face.slider<studio::house::Knob>("grip");auto* in=face->face.lever("in");auto* cover=face->face.button("setup");
    CHECK(grip!=nullptr&&in!=nullptr&&cover!=nullptr);
    if(grip==nullptr||in==nullptr||cover==nullptr)return;
    Pointer m{*r.stack};
    // Glass Current selected and Thread Press in view.
    const auto show=[&]{r.stack->select(ids[1]);r.stack->reveal(ids[2]);pump();};
    const auto at=[&](juce::Component& c,Pt p){return r.stack->getLocalPoint(&c,p);};
    const auto held=[&](Pt from){m.press(from);m.move(from+Pt(0.0f,-10.0f));m.move(from+Pt(0.0f,-20.0f));return r.stack->dragging();};
    {
        show();
        const auto t=idx("effect.4.compressor.threshold_db");const auto was=r.p.control(t).getValue();
        const auto from=at(*grip,local(*grip,grip->element.pivot));
        CHECK(!held(from));
        m.release(from+Pt(0.0f,-20.0f));
        CHECK(!r.stack->dragging()&&r.p.control(t).getValue()>was&&r.stack->selected()==ids[2]);
    }
    {
        show();
        const auto b=idx("effect.4.bypassed");const auto was=r.p.control(b).getValue();
        const auto from=at(*in,local(*in,in->element.rect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&r.p.control(b).getValue()!=was);
    }
    {
        show();
        const auto from=at(*cover,local(*cover,cover->element.rect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&face->isOpen());
        face->setOpen(false);pump();
    }
    // The brass plate, and the left ear between its slots.
    for(const auto spot:{Pt(290.0f,112.0f),Pt(40.0f,200.0f)})
    {
        show();juce::Component::unfocusAllComponents();pump();
        const auto from=at(face->face,spot);
        m.press(from);
        CHECK(r.stack->selected()==ids[2]&&!face->hasKeyboardFocus(true));
        m.move(from+Pt(0.0f,-20.0f));
        CHECK(r.stack->dragging());
        m.escape();m.release(m.last);
        CHECK(!r.stack->dragging());
    }
}
// Folding (the maker faceplates spec: The host's parts on a faceplate, The SET-UP plate): the triangle folds Thread Press
// to the host's fallback strip, closing its open SET-UP plate and writing nothing; unfolded, the faceplate shows closed.
// With --images: stack-faceplate-open.png.
void stackFaceplateFold()
{
    std::cout<<"SECTION stack_faceplate_fold\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* press=r.unit(2);auto* face=press!=nullptr?press->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();
    face->setOpen(true);pump();
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-faceplate-open.png"),r.body()));
    const auto before=r.revisions();
    pressRail(press->fold);pump();
    CHECK(press->isFolded()&&press->units()==1&&std::abs(r.at(*press).getHeight()-u)<=1.0f&&!face->isShowing()&&!face->isOpen()
          &&!press->operations.isShowing()&&r.revisions()==before);
    // The strip's LED, lit while the rack's banks are on, 40 design px past its name's tape.
    const auto tapeW=std::max(180.0f,studio::house::TapeArt::widthFor(r.model.house,"THREAD PRESS",54.0f));
    CHECK(meanLuma(snap(*press),Rect(8.0f,8.0f).withCentre(Pt((194.0f+tapeW+40.0f)*k,(float)press->getHeight()*0.5f)))>150.0);
    pressRail(press->fold);pump();
    CHECK(!press->isFolded()&&press->face()==face&&face->isShowing()&&!face->isOpen()&&press->operations.isShowing()&&!press->row.isShowing()
          &&r.revisions()==before);
}
// AI: KEEP and the lock dots (the maker faceplates spec: The host's parts on a faceplate): the tape crosses the
// faceplate's left ear while one of Thread Press's banks is AI-locked; a locked control shows the yellow dot at its place
// in the skin, a set-up control only while the plate is open. With --images: stack-faceplate-keep.png.
void stackFaceplateKeep()
{
    std::cout<<"SECTION stack_faceplate_keep\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* press=r.unit(2);auto* face=press!=nullptr?press->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    const auto k=r.stack->scale();
    const auto yellow=[&](Rect box)
    {
        const auto img=snap(*press);const auto b=box.getSmallestIntegerContainer().getIntersection(img.getBounds());int n=0;
        for(int y=b.getY();y<b.getBottom();++y)for(int x=b.getX();x<b.getRight();++x)
        {const auto p=img.getPixelAt(x,y);n+=p.getRed()>180&&p.getGreen()>150&&p.getBlue()<120?1:0;}
        return n;
    };
    const Rect ear(0.0f,0.0f,190.0f*k,90.0f*k);
    CHECK(!press->kept()&&yellow(ear)==0);
    auto d=document(r.p);d.effectLocks[3]=true;CHECK(edit(r.p,d));r.refresh();pump();
    CHECK(press->kept()&&yellow(ear)>500);
    // GRIP's and RATIO's parameters locked, and FRONT's not.
    d=document(r.p);d.controlLocks[idx("effect.4.compressor.threshold_db")]=true;d.controlLocks[idx("effect.4.compressor.ratio")]=true;
    CHECK(edit(r.p,d));r.refresh();pump();
    const auto dot=[&](studio::house::View& v,const char* id)
    {
        auto* knob=v.slider<studio::house::Knob>(id);
        CHECK(knob!=nullptr&&knob->element.hasDot);
        return knob!=nullptr?yellow(Rect(18.0f,18.0f).withCentre(press->getLocalPoint(&v,knob->element.dot))):-1;
    };
    const auto grip=dot(face->face,"grip"),front=dot(face->face,"front"),closed=dot(face->revealed(),"ratio");
    face->setOpen(true);pump();
    const auto open=dot(face->revealed(),"ratio");
    CHECK(grip>40&&front<5&&closed<5&&open>40);
    std::cout<<"  lock dots: GRIP "<<grip<<", FRONT "<<front<<", RATIO closed "<<closed<<", open "<<open<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-faceplate-keep.png"),r.body()));
}
// The AI's marks on a faceplate (the maker faceplates spec: The AI's marks): the stack hands the proposal to Thread
// Press's faceplate, not its hidden row; the same candidate again draws nothing; the live GRIP reaching the proposed value
// takes its mark; folded, the tape has the outline; another panel outlines the faceplate, in the unit's description; none
// clears them. An edit makes the editor's proposal out of date, which reaches the stack as none (editor_proposal_marks).
// With --images: stack-faceplate-marks.png and stack-faceplate-folded.png.
void stackFaceplateMarks()
{
    std::cout<<"SECTION stack_faceplate_marks\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* press=r.unit(2);auto* face=press!=nullptr?press->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    auto* grip=face->face.slider<studio::house::Knob>("grip");auto* in=face->face.lever("in");
    CHECK(grip!=nullptr&&in!=nullptr);
    if(grip==nullptr||in==nullptr)return;
    const auto t=grip->element.paramIndex,b=in->element.paramIndex;
    auto candidate=document(r.p);candidate.values[t]=r.p.control(t).convertTo0to1(-24.0f);candidate.values[b]=1.0f;
    r.stack->setProposal(&candidate);pump();
    CHECK(face->marks().size()==2&&!face->proposesPanel()&&press->proposed()&&press->row.marks().empty()&&!press->row.proposesPanel());
    CHECK(grip->getDescription()=="AI proposes -24.0 dB"&&in->getDescription()=="AI proposes OUT"&&!press->getDescription().contains("AI proposes"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-faceplate-marks.png"),r.body()));
    const auto copy=candidate;
    CHECK(paintsOver(*face,[&]{for(int n=0;n<20;++n){r.stack->setProposal(&copy);r.refresh();pump(25);}})==0);
    {auto d=document(r.p);d.values[t]=candidate.values[t];CHECK(edit(r.p,d));r.refresh();pump();}
    CHECK(face->marks().size()==1&&face->marks()[0].element=="face.in"&&grip->getDescription().isEmpty()&&press->proposed());
    press->fold.onToggle();pump();
    CHECK(press->isFolded()&&tapeOutline(*r.stack,*press)>=40);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-faceplate-folded.png"),r.body()));
    press->fold.onToggle();pump();
    auto other=candidate;auto racks=*other.personalities;
    for(auto& i:racks.instances)if(i.id==press->id())i.design.panel.controls[0].label="Hold";
    other.personalities=std::make_shared<const RackPersonalityState>(std::move(racks));
    r.stack->setProposal(&other);pump();
    CHECK(face->proposesPanel()&&face->marks().empty()&&press->proposed()&&press->getDescription().endsWith("\nAI proposes a new panel"));
    r.stack->setProposal(nullptr);pump();
    CHECK(!face->proposesPanel()&&face->marks().empty()&&!press->proposed()&&!press->getDescription().contains("AI proposes")&&in->getDescription().isEmpty());
}
// The LIBRARY's preview of Thread Press (the maker faceplates spec, decision 8): its faceplate composed at the design's
// values, dimmed to half, under the dashes, with no identity block or row; its letters in the middle on a dark band.
// With --images: stack-faceplate-preview.png.
void stackFaceplatePreview()
{
    std::cout<<"SECTION stack_faceplate_preview\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    const auto thread=airack::test::frozenExampleRackDesigns()[2];const auto skin=studio::makerFaceFor(thread);
    CHECK(skin!=nullptr);
    if(skin==nullptr)return;
    const StackPlacement preview{{RackRole::fx,masterChain,1},"PREVIEW: SHARED MASTER, AFTER THREAD PRESS",rackPreviewInstance(thread),rackPreviewValues(thread)};
    r.stack->setPlacement(preview);pump();
    auto* s=r.stack->placement();
    CHECK(s!=nullptr);
    if(s==nullptr)return;
    const auto k=r.stack->scale();const auto place=r.at(*s);
    CHECK(s->getTitle()==preview.letters&&s->getDescription()=="Thread Press"&&std::abs(place.getHeight()-2.0f*r.stack->unitHeight())<=1.0f);
    auto* air=find(*s,"rack.focus.0");
    CHECK(!lettered(*s,"THREAD PRESS")&&!lettered(*s,preview.letters)&&(air==nullptr||!air->isShowing()));
    const auto composed=studio::MakerFace::composeFor(*skin,*preview.preview,rackPreviewParameters(thread));
    CHECK(composed.getWidth()==2140&&composed.getHeight()==394);
    if(!composed.isValid())return;
    // Away from the dashes and the band, half the composed face over the seam.
    const auto img=r.body();
    for(const auto area:{Rect(250.0f,60.0f,100.0f,40.0f),Rect(1200.0f,300.0f,150.0f,50.0f),Rect(1700.0f,40.0f,150.0f,40.0f)})
    {
        const auto shown=meanLuma(img,(area*k).translated((float)studio::HouseUnit::railW*k,place.getY())),drawn=meanLuma(composed,area);
        CHECK(std::abs(shown-(drawn*0.5+2.0))<8.0);
        std::cout<<"  "<<area.toString()<<": shown "<<shown<<", composed "<<drawn<<'\n';
    }
    // The letters, light, over a dark band: the pixels between them, clear of the letters' edges, are near black.
    const auto middle=Rect(773.0f-150.0f,place.getCentreY()-10.0f,300.0f,20.0f).getSmallestIntegerContainer();
    double between=0;int n=0;
    for(int y=middle.getY();y<middle.getBottom();++y)for(int x=middle.getX();x<middle.getRight();++x)
        if(const auto l=luma(img.getPixelAt(x,y));l<60.0){between+=l;++n;}
    CHECK(peakLuma(img,middle.toFloat())>150.0&&n>0&&between/n<25.0);
    std::cout<<"  band between the letters: "<<(n>0?between/n:-1.0)<<", composed there "<<meanLuma(composed,(middle.toFloat().translated(0.0f,-place.getY())/k).translated(-(float)studio::HouseUnit::railW,0.0f))*0.5<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-faceplate-preview.png"),img));
}
// Ember Reed's faceplate in the stack (the Ember Reed faceplate spec: Ember Reed closed, The host's parts on a faceplate,
// Keyboard and accessibility): EMBER REED is a 3U unit showing HALLORAN's faceplate between the rails, bound to its own
// banks, instead of its identity block, UP, DOWN and row; Tab runs its triangle, the face's six controls, the pull and
// OPERATIONS; a press on AIR, BLOOM or the pull never drags, and one on a chrome handle or the bare plate selects it and a
// 20 px drag lifts it. With --images: stack-ember.png.
void stackEmber()
{
    std::cout<<"SECTION stack_ember\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);auto* face=ember!=nullptr?ember->face():nullptr;
    CHECK(face!=nullptr&&glass!=nullptr&&face->front()!=nullptr);
    if(face==nullptr||glass==nullptr||face->front()==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();
    CHECK(find(*ember,"rack.face")==face&&face->isShowing()&&!face->isOpen()&&ember->units()==3&&std::abs(r.at(*ember).getHeight()-3.0f*u)<=1.0f);
    CHECK(closeTo(r.at(*face),Rect((float)studio::HouseUnit::railW*k,r.at(*ember).getY(),2140.0f*k,591.0f*k)));
    CHECK(!lettered(*ember,"EMBER REED")&&!lettered(*ember,"SYNTH 1")&&!ember->up.isShowing()&&!ember->down.isShowing()&&!ember->row.isShowing()
          &&ember->operations.isShowing());
    const auto d=document(r.p);
    CHECK(ember->getDescription()==rackHeaderText(d,d.personalities->instances[0])+"\nHALLORAN ELECTRONIC MUSIC CO., ANALOG VOICE MODEL 74");
    // Its own banks: the analog voice in synth bank 1, the drive in effect bank 1.
    auto* air=face->face.slider<studio::house::Knob>("air");auto* bloom=face->face.slider<studio::house::Fader>("bloom");
    auto* grain=face->face.slider<studio::house::Knob>("grain");auto* pull=face->front()->button("programmer");
    CHECK(air!=nullptr&&bloom!=nullptr&&grain!=nullptr&&pull!=nullptr);
    if(air==nullptr||bloom==nullptr||grain==nullptr||pull==nullptr)return;
    CHECK(air->element.paramIndex==idx("synth.1.analog.filter.cutoff")&&bloom->element.paramIndex==idx("synth.1.analog.filter_env.decay")
          &&grain->element.paramIndex==idx("effect.1.drive.gain_db"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-ember.png"),r.body()));
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(ember->isParentOf(c))order.add(c->getComponentID());
    const juce::StringArray want{"rack.fold","face.air","face.bite","face.bloom","face.attack","face.release","face.grain","face.programmer","rack.operations"};
    CHECK(order==want);
    if(order!=want)std::cout<<"  Tab order: "<<order.joinIntoString(" ")<<'\n';
    // Glass Current selected and Ember Reed in view; each press 20 px up.
    const auto ids=r.ids();
    Pointer m{*r.stack};
    const auto show=[&]{r.stack->select(ids[1]);r.stack->reveal(ids[0]);pump();};
    const auto at=[&](juce::Component& c,Pt p){return r.stack->getLocalPoint(&c,p);};
    const auto held=[&](Pt from){m.press(from);m.move(from+Pt(0.0f,-10.0f));m.move(from+Pt(0.0f,-20.0f));return r.stack->dragging();};
    for(auto* slider:std::initializer_list<juce::Slider*>{air,bloom})
    {
        show();
        auto* control=dynamic_cast<studio::house::BoundSlider*>(slider);
        const auto index=control!=nullptr?control->element.paramIndex:0;const auto was=r.p.control(index).getValue();
        const auto from=at(*slider,slider==air?local(*air,air->element.pivot):local(*bloom,{bloom->element.capCentreX,studio::house::Fader::capYAt(bloom->element,(float)bloom->proportion())}));
        CHECK(!held(from));
        m.release(from+Pt(0.0f,-20.0f));
        CHECK(!r.stack->dragging()&&r.p.control(index).getValue()>was&&r.stack->selected()==ids[0]);
    }
    {
        show();
        const auto from=at(*pull,local(*pull,pull->element.rect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&face->isOpen());
        face->setOpen(false);pump();
    }
    // The left chrome handle, and the bare plate between BITE and BLOOM.
    for(const auto spot:{Pt(86.0f,230.0f),Pt(1080.0f,120.0f)})
    {
        show();juce::Component::unfocusAllComponents();pump();
        const auto from=at(face->face,spot);
        m.press(from);
        CHECK(r.stack->selected()==ids[0]&&!face->hasKeyboardFocus(true));
        m.move(from+Pt(0.0f,-20.0f));
        CHECK(r.stack->dragging());
        m.escape();m.release(m.last);
        CHECK(!r.stack->dragging());
    }
}
// The PROGRAMMER drawer in the stack (the Ember Reed faceplate spec, decision 8): the pull makes the unit 7U, moves the
// units under it down by 4U and scrolls the least that shows the deck and the front, writing nothing; Tab reaches the
// deck's controls; the pull closes it and the units move back; folding closes it and unfolding shows it closed; an edit
// that takes the rack off its faceplate while it is open leaves a 2U house plate. With --images: stack-ember-open.png,
// stack-ember-folded.png and stack-ember-tape.png, VEL TO AIR's tape while route 1 is from LFO 1.
void stackEmberDrawer()
{
    std::cout<<"SECTION stack_ember_drawer\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);auto* press=r.unit(2);auto* face=ember!=nullptr?ember->face():nullptr;
    auto* pull=face!=nullptr&&face->front()!=nullptr?face->front()->button("programmer"):nullptr;
    CHECK(pull!=nullptr&&glass!=nullptr&&press!=nullptr&&r.divider()!=nullptr);
    if(pull==nullptr||glass==nullptr||press==nullptr||r.divider()==nullptr)return;
    const auto u=r.stack->unitHeight();
    const auto before=r.revisions();
    const auto glassTop=r.at(*glass).getY(),dividerTop=r.at(*r.divider()).getY(),pressTop=r.at(*press).getY();
    const auto moved=[&](float by){return std::abs(r.at(*glass).getY()-(glassTop+by))<=1.0f&&std::abs(r.at(*r.divider()).getY()-(dividerTop+by))<=1.0f
                                          &&std::abs(r.at(*press).getY()-(pressTop+by))<=1.0f;};
    auto& view=r.stack->viewport();
    view.setViewPosition(0,0);pump();
    pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    CHECK(face->isOpen()&&ember->units()==7&&std::abs(r.at(*ember).getHeight()-7.0f*u)<=1.0f&&moved(4.0f*u)&&r.revisions()==before);
    // The deck and the front, 5U, fit the view: it moves the least that shows them.
    const auto shown=r.at(face->revealed()).getUnion(r.at(*face->front()));
    CHECK(r.inView(face->revealed())&&r.inView(*face->front())&&std::abs((float)view.getViewPositionY()-(shown.getBottom()-(float)view.getHeight()))<=1.0f);
    std::cout<<"  open: deck and front "<<shown.toString()<<", view at "<<view.getViewPositionY()<<'\n';
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(ember->isParentOf(c))order.add(c->getComponentID());
    const juce::StringArray want{"rack.fold","face.air","face.bite","face.bloom","face.attack","face.release","face.grain",
        "face.osc1.wave","face.osc1.tune","face.osc1.fine","face.osc1.width","face.osc1.level","face.osc2.wave","face.osc2.tune","face.osc2.fine",
        "face.osc2.level","face.osc2.noise","face.fenv.mode","face.fenv.amount","face.fenv.attack","face.fenv.sustain","face.fenv.release",
        "face.amp.decay","face.amp.sustain","face.lfo.rate","face.lfo.shape","face.vel-air","face.lfo.sync","face.drive.shape","face.drive.tone",
        "face.drive.mix","face.programmer","rack.operations"};
    CHECK(order==want);
    if(order!=want)std::cout<<"  open Tab order: "<<order.joinIntoString(" ")<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-ember-open.png"),r.body()));
    pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    CHECK(!face->isOpen()&&ember->units()==3&&moved(0.0f)&&r.revisions()==before);
    // Folding closes it; unfolded, it shows closed.
    pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    pressRail(ember->fold);pump();
    CHECK(ember->isFolded()&&!face->isOpen()&&!face->isShowing()&&ember->units()==1&&moved(-2.0f*u)&&r.revisions()==before);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-ember-folded.png"),r.body()));
    pressRail(ember->fold);pump();
    CHECK(!ember->isFolded()&&ember->face()==face&&face->isShowing()&&!face->isOpen()&&ember->units()==3&&moved(0.0f)&&r.revisions()==before);
    // Route 1 from LFO 1: the open deck's tape over VEL TO AIR's legend.
    {
        auto d=document(r.p);d.values[idx("synth.1.modulation.1.source")]=r.p.control(idx("synth.1.modulation.1.source")).convertTo0to1(3.0f);
        CHECK(edit(r.p,d));r.refresh();pump();
        pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
        CHECK(face->isOpen()&&face->routeTape()=="LFO 1 TO CUTOFF");
        if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-ember-tape.png"),r.body()));
    }
    // Open, a relabelled control takes the rack off its faceplate: a 2U house plate, the units under it 1U higher.
    {
        const auto d=document(r.p);const auto& i=d.personalities->instances[0];
        auto glow=i.design.panel.controls[0];glow.label="Glow";
        RackOperation op;op.kind=RackOperationKind::move;op.selected=i.id;op.destination={RackRole::synth,masterChain,0};op.choices.controls.push_back({i.id,0,glow});
        SoundDocument now;std::uint64_t revision=0;
        CHECK(r.p.trySoundSnapshot(now,revision)&&r.p.applyRackOperation(op,revision)==RackCandidateStatus::ready);r.refresh();pump();
        CHECK(ember->face()==nullptr&&find(*ember,"rack.face")==nullptr&&ember->units()==2&&std::abs(r.at(*ember).getHeight()-2.0f*u)<=1.0f
              &&ember->row.isShowing()&&moved(-u));
    }
}
// AI: KEEP and the lock dots on Ember Reed (the Ember Reed faceplate spec: The host's parts on a faceplate): the tape
// crosses the faceplate's left ear while its analog bank is AI-locked; AIR's dot shows, and LFO RATE's only while the
// drawer is open. With --images: stack-ember-keep.png.
void stackEmberKeep()
{
    std::cout<<"SECTION stack_ember_keep\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* face=ember!=nullptr?ember->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    const auto k=r.stack->scale();
    const auto yellow=[&](Rect box)
    {
        const auto img=snap(*ember);const auto b=box.getSmallestIntegerContainer().getIntersection(img.getBounds());int n=0;
        for(int y=b.getY();y<b.getBottom();++y)for(int x=b.getX();x<b.getRight();++x)
        {const auto p=img.getPixelAt(x,y);n+=p.getRed()>180&&p.getGreen()>150&&p.getBlue()<120?1:0;}
        return n;
    };
    const Rect ear(0.0f,0.0f,190.0f*k,90.0f*k);
    CHECK(!ember->kept()&&yellow(ear)==0);
    auto d=document(r.p);d.synthLocks[0]=true;CHECK(edit(r.p,d));r.refresh();pump();
    CHECK(ember->kept()&&yellow(ear)>500);
    d=document(r.p);d.controlLocks[idx("synth.1.analog.filter.cutoff")]=true;d.controlLocks[idx("synth.1.lfo.1.rate_hz")]=true;
    CHECK(edit(r.p,d));r.refresh();pump();
    const auto dot=[&](studio::house::View& v,const char* id)
    {
        auto* knob=v.slider<studio::house::Knob>(id);
        CHECK(knob!=nullptr&&knob->element.hasDot);
        return knob!=nullptr?yellow(Rect(18.0f,18.0f).withCentre(ember->getLocalPoint(&v,knob->element.dot))):-1;
    };
    const auto air=dot(face->face,"air"),bite=dot(face->face,"bite");
    face->setOpen(true);pump();
    const auto rate=dot(face->revealed(),"lfo.rate"),tune=dot(face->revealed(),"osc1.tune");
    face->setOpen(false);pump();
    const auto closed=yellow(Rect(0.0f,0.0f,(float)ember->getWidth(),(float)ember->getHeight()).withTrimmedTop(394.0f*k));
    CHECK(air>40&&bite<5&&rate>40&&tune<5&&closed<5);
    std::cout<<"  lock dots: AIR "<<air<<", BITE "<<bite<<", LFO RATE open "<<rate<<", OSC 1 TUNE "<<tune<<", the front closed "<<closed<<'\n';
    face->setOpen(true);pump();
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-ember-keep.png"),r.body()));
}
// The AI's marks on Ember Reed (the Ember Reed faceplate spec: The host's parts on a faceplate): the stack hands the
// proposal to the faceplate; closed, the deck's mark waits behind the pull's outline and its description; folded, the
// tape has the outline; none clears them. With --images: stack-ember-marks.png and stack-ember-marks-open.png.
void stackEmberMarks()
{
    std::cout<<"SECTION stack_ember_marks\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* face=ember!=nullptr?ember->face():nullptr;
    auto* pull=face!=nullptr&&face->front()!=nullptr?face->front()->button("programmer"):nullptr;
    CHECK(pull!=nullptr);
    if(pull==nullptr)return;
    const auto air=idx("synth.1.analog.filter.cutoff"),rate=idx("synth.1.lfo.1.rate_hz");
    auto candidate=document(r.p);candidate.values[air]=r.p.control(air).convertTo0to1(2000.0f);candidate.values[rate]=r.p.control(rate).convertTo0to1(3.0f);
    r.stack->setProposal(&candidate);pump();
    CHECK(face->marks().size()==2&&!face->proposesPanel()&&ember->proposed()&&ember->row.marks().empty()&&!ember->row.proposesPanel());
    CHECK(pull->getDescription()=="AI proposes changes to 1 programmer control"&&!ember->getDescription().contains("AI proposes"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-ember-marks.png"),r.body()));
    face->setOpen(true);pump();
    CHECK(pull->getDescription().isEmpty()&&face->revealed().slider<studio::house::Knob>("lfo.rate")!=nullptr
          &&face->revealed().slider<studio::house::Knob>("lfo.rate")->getDescription()=="AI proposes 3.00 Hz");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-ember-marks-open.png"),r.body()));
    face->setOpen(false);pump();
    ember->fold.onToggle();pump();
    CHECK(ember->isFolded()&&tapeOutline(*r.stack,*ember)>=40);
    ember->fold.onToggle();pump();
    r.stack->setProposal(nullptr);pump();
    CHECK(face->marks().empty()&&!ember->proposed()&&pull->getDescription().isEmpty());
}
// The LIBRARY's preview of Ember Reed (the Ember Reed faceplate spec, decision 11): its closed faceplate composed at the
// design's values, 3U, dimmed to half, under the dashes, with its letters on a dark band; the units under it move by 3U.
// With --images: stack-ember-preview.png.
void stackEmberPreview()
{
    std::cout<<"SECTION stack_ember_preview\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    if(r.divider()==nullptr)return;
    const auto ember=airack::test::frozenExampleRackDesigns()[0];const auto skin=studio::makerFaceFor(ember);
    CHECK(skin!=nullptr);
    if(skin==nullptr)return;
    const auto u=r.stack->unitHeight();const auto dividerTop=r.at(*r.divider()).getY();
    const StackPlacement preview{{RackRole::synth,masterChain,2},"PREVIEW: SYNTH 3, AFTER GLASS CURRENT",rackPreviewInstance(ember),rackPreviewValues(ember)};
    r.stack->setPlacement(preview);pump();
    auto* s=r.stack->placement();
    CHECK(s!=nullptr);
    if(s==nullptr)return;
    const auto k=r.stack->scale();const auto place=r.at(*s);
    CHECK(s->getTitle()==preview.letters&&s->getDescription()=="Ember Reed"&&std::abs(place.getHeight()-3.0f*u)<=1.0f
          &&std::abs(r.at(*r.divider()).getY()-(dividerTop+3.0f*u))<=1.0f);
    auto* first=find(*s,"rack.focus.0");
    CHECK(!lettered(*s,"EMBER REED")&&!lettered(*s,preview.letters)&&(first==nullptr||!first->isShowing()));
    const auto composed=studio::MakerFace::composeFor(*skin,*preview.preview,rackPreviewParameters(ember));
    CHECK(composed.getWidth()==2140&&composed.getHeight()==591);
    if(!composed.isValid())return;
    const auto img=r.body();
    for(const auto area:{Rect(250.0f,60.0f,100.0f,40.0f),Rect(1700.0f,40.0f,150.0f,40.0f),Rect(1300.0f,480.0f,200.0f,50.0f)})
    {
        const auto shown=meanLuma(img,(area*k).translated((float)studio::HouseUnit::railW*k,place.getY())),drawn=meanLuma(composed,area);
        CHECK(std::abs(shown-(drawn*0.5+2.0))<8.0);
        std::cout<<"  "<<area.toString()<<": shown "<<shown<<", composed "<<drawn<<'\n';
    }
    const auto middle=Rect(773.0f-150.0f,place.getCentreY()-10.0f,300.0f,20.0f).getSmallestIntegerContainer();
    double between=0;int n=0;
    for(int y=middle.getY();y<middle.getBottom();++y)for(int x=middle.getX();x<middle.getRight();++x)
        if(const auto l=luma(img.getPixelAt(x,y));l<60.0){between+=l;++n;}
    CHECK(peakLuma(img,middle.toFloat())>150.0&&n>0&&between/n<25.0);
    if(output!=juce::File{}){r.stack->viewport().setViewPosition(0,(int)place.getY()-200);pump();CHECK(writePng(output.getChildFile("stack-ember-preview.png"),r.body()));}
}
// Glass Current's faceplate in the stack (the Glass Current faceplate spec: Glass Current closed, The host's parts on a
// faceplate, Keyboard and accessibility): GLASS CURRENT is a 2U unit showing KUROSHIO's faceplate between the rails,
// bound to its own banks, instead of its identity block, UP, DOWN and row; Tab runs its triangle, the face's four knobs,
// the top notch and OPERATIONS; a press on FACET or the notch never drags, and one on the left ear or the bare plate
// selects it and a 20 px drag lifts it. With --images: stack-glass.png.
void stackGlass()
{
    std::cout<<"SECTION stack_glass\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* glass=r.unit(1);auto* face=glass!=nullptr?glass->face():nullptr;
    CHECK(face!=nullptr&&face->hanging()!=nullptr);
    if(face==nullptr||face->hanging()==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();
    CHECK(find(*glass,"rack.face")==face&&face->isShowing()&&!face->isOpen()&&glass->units()==2&&std::abs(r.at(*glass).getHeight()-2.0f*u)<=1.0f);
    CHECK(closeTo(r.at(*face),Rect((float)studio::HouseUnit::railW*k,r.at(*glass).getY(),2140.0f*k,394.0f*k)));
    CHECK(!lettered(*glass,"GLASS CURRENT")&&!lettered(*glass,"SYNTH 2")&&!glass->up.isShowing()&&!glass->down.isShowing()&&!glass->row.isShowing()
          &&glass->operations.isShowing());
    const auto d=document(r.p);
    CHECK(glass->getDescription()==rackHeaderText(d,d.personalities->instances[1])+"\nKUROSHIO, GLASS CURRENT GC-86 DIGITAL WAVETABLE SYNTHESIZER");
    // Its own banks: the wavetable voice in synth bank 2, the delay in effect bank 2 and the reverb in bank 3.
    auto* facet=face->face.slider<studio::house::Knob>("facet");auto* echo=face->face.slider<studio::house::Knob>("echo");
    auto* horizon=face->face.slider<studio::house::Knob>("horizon");auto* edit=face->face.button("edit");
    CHECK(facet!=nullptr&&echo!=nullptr&&horizon!=nullptr&&edit!=nullptr);
    if(facet==nullptr||echo==nullptr||horizon==nullptr||edit==nullptr)return;
    CHECK(facet->element.paramIndex==idx("synth.2.wavetable.osc.1.position")&&echo->element.paramIndex==idx("effect.2.delay.mix")
          &&horizon->element.paramIndex==idx("effect.3.reverb.decay_seconds"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-glass.png"),r.body()));
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(glass->isParentOf(c))order.add(c->getComponentID());
    const juce::StringArray want{"rack.fold","face.facet","face.drift","face.echo","face.horizon","face.edit","rack.operations"};
    CHECK(order==want);
    if(order!=want)std::cout<<"  Tab order: "<<order.joinIntoString(" ")<<'\n';
    // Ember Reed selected and Glass Current in view; each press 20 px up.
    const auto ids=r.ids();
    Pointer m{*r.stack};
    const auto show=[&]{r.stack->select(ids[0]);r.stack->reveal(ids[1]);pump();};
    const auto at=[&](juce::Component& c,Pt p){return r.stack->getLocalPoint(&c,p);};
    const auto held=[&](Pt from){m.press(from);m.move(from+Pt(0.0f,-10.0f));m.move(from+Pt(0.0f,-20.0f));return r.stack->dragging();};
    {
        show();
        const auto index=facet->element.paramIndex;const auto was=r.p.control(index).getValue();
        const auto from=at(*facet,local(*facet,facet->element.pivot));
        CHECK(!held(from));
        m.release(from+Pt(0.0f,-20.0f));
        CHECK(!r.stack->dragging()&&r.p.control(index).getValue()>was&&r.stack->selected()==ids[1]);
    }
    {
        show();
        const auto from=at(*edit,local(*edit,edit->element.rect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&face->isOpen());
        face->setOpen(false);pump();
    }
    // The left ear, and the bare plate between DRIFT and ECHO under their ticks.
    for(const auto spot:{Pt(40.0f,200.0f),Pt(1166.0f,320.0f)})
    {
        show();juce::Component::unfocusAllComponents();pump();
        const auto from=at(face->face,spot);
        m.press(from);
        CHECK(r.stack->selected()==ids[1]&&!face->hasKeyboardFocus(true));
        m.move(from+Pt(0.0f,-20.0f));
        CHECK(r.stack->dragging());
        m.escape();m.release(m.last);
        CHECK(!r.stack->dragging());
    }
}
// The EDIT door in the stack (the Glass Current faceplate spec, decisions 3 and 10): the top notch makes the unit 4U,
// moves the units under it down by 2U and scrolls the least that shows the door, writing nothing; Tab reaches the chosen
// page's button and its screen's items; the foot notch closes it and the units move back; folding closes it and
// unfolding shows it closed; an edit that takes the rack off its faceplate while it is open leaves a 2U house plate, the
// units under it back up. With --images: stack-glass-open.png, the door on DELAY, and stack-glass-vfd.png, the VFD after
// REVERB SIZE is touched.
void stackGlassDoor()
{
    std::cout<<"SECTION stack_glass_door\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* glass=r.unit(1);auto* press=r.unit(2);auto* face=glass!=nullptr?glass->face():nullptr;
    auto* edit=face!=nullptr?face->face.button("edit"):nullptr;
    auto* close=face!=nullptr&&face->hanging()!=nullptr?face->hanging()->button("close"):nullptr;
    CHECK(edit!=nullptr&&close!=nullptr&&press!=nullptr&&r.divider()!=nullptr);
    if(edit==nullptr||close==nullptr||press==nullptr||r.divider()==nullptr)return;
    const auto u=r.stack->unitHeight();
    const auto before=r.revisions();
    const auto dividerTop=r.at(*r.divider()).getY(),pressTop=r.at(*press).getY();
    const auto moved=[&](float by){return std::abs(r.at(*r.divider()).getY()-(dividerTop+by))<=1.0f&&std::abs(r.at(*press).getY()-(pressTop+by))<=1.0f;};
    const auto space=juce::KeyPress(juce::KeyPress::spaceKey);
    auto& view=r.stack->viewport();
    view.setViewPosition(0,0);pump();
    edit->keyPressed(space);pump();
    CHECK(face->isOpen()&&glass->units()==4&&std::abs(r.at(*glass).getHeight()-4.0f*u)<=1.0f&&moved(2.0f*u)&&r.revisions()==before);
    // The door, under the face, fits the view: it moves the least that shows it.
    const auto door=r.stack->body().getLocalArea(face,face->openedArea().toFloat());
    CHECK(!door.isEmpty()&&door.getY()>=(float)view.getViewPositionY()-0.5f&&std::abs((float)view.getViewPositionY()-(door.getBottom()-(float)view.getHeight()))<=1.0f);
    std::cout<<"  open: door "<<door.toString()<<", view at "<<view.getViewPositionY()<<'\n';
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(glass->isParentOf(c))order.add(c->getComponentID());
    const juce::StringArray want{"rack.fold","face.facet","face.drift","face.echo","face.horizon","face.page.osc","face.osc1.table","face.osc1.pos",
        "face.osc1.tune","face.osc1.fine","face.close","rack.operations"};
    CHECK(order==want);
    if(order!=want)std::cout<<"  open Tab order: "<<order.joinIntoString(" ")<<'\n';
    auto* delay=face->revealed().button("page.delay");auto* reverb=face->revealed().button("page.reverb");
    CHECK(delay!=nullptr&&reverb!=nullptr);
    if(delay!=nullptr&&reverb!=nullptr)
    {
        delay->keyPressed(space);pump();
        CHECK(face->screen()=="delay.1"&&r.revisions()==before);
        if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-glass-open.png"),r.body()));
        reverb->keyPressed(space);pump();
        auto* size=face->revealed().slider<studio::house::Knob>("reverb.size");
        CHECK(size!=nullptr);
        if(size!=nullptr)size->grabKeyboardFocus();
        pump();
        CHECK(face->vfd().line1.startsWith("REVERB SIZE ")&&r.revisions()==before);
        if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-glass-vfd.png"),r.body()));
    }
    close->keyPressed(space);pump();
    CHECK(!face->isOpen()&&glass->units()==2&&moved(0.0f)&&r.revisions()==before);
    // Folding closes it; unfolded, it shows closed.
    edit->keyPressed(space);pump();
    pressRail(glass->fold);pump();
    CHECK(glass->isFolded()&&!face->isOpen()&&glass->units()==1&&moved(-u)&&r.revisions()==before);
    pressRail(glass->fold);pump();
    CHECK(!glass->isFolded()&&glass->face()==face&&face->face.isShowing()&&!face->isOpen()&&glass->units()==2&&moved(0.0f)&&r.revisions()==before);
    // Open, a relabelled control takes the rack off its faceplate: a 2U house plate, the units under it back up.
    edit->keyPressed(space);pump();
    CHECK(face->isOpen()&&moved(2.0f*u));
    {
        const auto d=document(r.p);const auto& i=d.personalities->instances[1];
        auto sky=i.design.panel.controls[0];sky.label="Sky";
        RackOperation op;op.kind=RackOperationKind::move;op.selected=i.id;op.destination={RackRole::synth,masterChain,1};op.choices.controls.push_back({i.id,0,sky});
        SoundDocument now;std::uint64_t revision=0;
        CHECK(r.p.trySoundSnapshot(now,revision)&&r.p.applyRackOperation(op,revision)==RackCandidateStatus::ready);r.refresh();pump();
        CHECK(glass->face()==nullptr&&find(*glass,"rack.face")==nullptr&&glass->units()==2&&std::abs(r.at(*glass).getHeight()-2.0f*u)<=1.0f
              &&glass->row.isShowing()&&moved(0.0f));
    }
}
// KUROSHIO's strip in the stack (the Glass Current faceplate spec, decision 9): the triangle folds GLASS CURRENT to the
// faceplate's own 1U strip, which shows as composeStrip draws it with no fallback strip under it, OPERATIONS hidden,
// writing nothing; AI: KEEP crosses its left ear while the synth bank is AI-locked; while the unit has marks, the strip's
// display is outlined and no tape is; unfolded, the closed face shows. With --images: stack-glass-marks.png and
// stack-glass-marks-open.png, the marks closed and open on DELAY, and stack-glass-folded.png, folded with them and
// AI: KEEP.
void stackGlassStrip()
{
    std::cout<<"SECTION stack_glass_strip\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* glass=r.unit(1);auto* face=glass!=nullptr?glass->face():nullptr;auto* strip=face!=nullptr?face->strip():nullptr;
    const studio::house::Element* name=nullptr;
    if(face!=nullptr)for(const auto& e:face->face.ctx.model.elements)if(e.id=="strip.name")name=&e;
    CHECK(strip!=nullptr&&name!=nullptr);
    if(strip==nullptr||name==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight(),railX=(float)studio::HouseUnit::railW*k;
    const auto before=r.revisions();
    pressRail(glass->fold);pump();
    CHECK(glass->isFolded()&&face->isFolded()&&glass->face()==face&&face->isShowing()&&strip->isShowing()&&!face->face.isShowing()&&glass->units()==1
          &&std::abs(r.at(*glass).getHeight()-u)<=1.0f&&!glass->operations.isShowing()&&r.revisions()==before);
    CHECK(closeTo(r.at(*face),Rect(railX,r.at(*glass).getY(),2140.0f*k,197.0f*k)));
    // The strip as composeStrip draws it: the wordmark, the display and the stripes.
    {
        const auto composed=face->composeStrip(document(r.p).values);const auto img=snap(*glass);
        CHECK(composed.getWidth()==2140&&composed.getHeight()==197);
        for(const auto area:{Rect(120.0f,50.0f,300.0f,90.0f),name->rect,Rect(1500.0f,40.0f,400.0f,110.0f)})
        {
            const auto shown=meanLuma(img,(area*k).translated(railX,0.0f)),drawn=meanLuma(composed,area);
            CHECK(std::abs(shown-drawn)<8.0);
            std::cout<<"  "<<area.toString()<<": shown "<<shown<<", composed "<<drawn<<'\n';
        }
    }
    // Under the faceplate the unit paints nothing: without it, the place of the fallback strip's tape is bare.
    face->setVisible(false);
    const auto bare=snap(*glass);
    face->setVisible(true);pump();
    CHECK(bare.getPixelAt(juce::roundToInt(284.0f*k),glass->getHeight()/2).getAlpha()==0);
    // AI: KEEP across the strip's left ear.
    const auto yellow=[&]
    {
        const auto img=snap(*glass);int n=0;
        for(int y=0;y<juce::roundToInt(90.0f*k);++y)for(int x=0;x<juce::roundToInt(190.0f*k);++x)
        {const auto p=img.getPixelAt(x,y);n+=p.getRed()>180&&p.getGreen()>150&&p.getBlue()<120?1:0;}
        return n;
    };
    const auto plain=yellow();
    {auto d=document(r.p);d.synthLocks[1]=true;CHECK(edit(r.p,d));r.refresh();pump();}
    CHECK(glass->isFolded()&&glass->kept()&&yellow()>plain+500);
    std::cout<<"  AI: KEEP: "<<plain<<" yellow px, then "<<yellow()<<'\n';
    // The near-white pixels on the band along the top of the display's outline.
    const auto ring=[&]
    {
        const auto img=snap(*glass);const auto top=(name->rect*k).translated(railX,0.0f).expanded(3.0f);int n=0;
        for(int x=juce::roundToInt(top.getX());x<juce::roundToInt(top.getRight());++x)
            for(int y=juce::roundToInt(top.getY())-2;y<=juce::roundToInt(top.getY())+2;++y)n+=luma(img.getPixelAt(x,y))>0.9*255.0?1:0;
        return n;
    };
    const auto plainRing=ring(),plainTape=tapeOutline(*r.stack,*glass);
    pressRail(glass->fold);pump();
    // FACET and DELAY FDBK proposed.
    const auto fa=idx("synth.2.wavetable.osc.1.position"),fb=idx("effect.2.delay.feedback");
    auto candidate=document(r.p);candidate.values[fa]=candidate.values[fa]<0.5f?candidate.values[fa]+0.35f:candidate.values[fa]-0.35f;
    candidate.values[fb]=candidate.values[fb]<0.5f?candidate.values[fb]+0.35f:candidate.values[fb]-0.35f;
    r.stack->setProposal(&candidate);pump();
    CHECK(!face->marks().empty()&&glass->proposed()&&!glass->getDescription().contains("AI proposes"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-glass-marks.png"),r.body()));
    if(auto* delay=face->revealed().button("page.delay"))
    {
        face->setOpen(true);delay->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
        CHECK(face->screen()=="delay.1");
        if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-glass-marks-open.png"),r.body()));
        face->setOpen(false);pump();
    }
    else CHECK(false);
    pressRail(glass->fold);pump();
    const auto marked=ring(),tape=tapeOutline(*r.stack,*glass);
    CHECK(glass->isFolded()&&marked>plainRing+40&&tape<plainTape+10);
    std::cout<<"  display outline: "<<plainRing<<" then "<<marked<<"; tape band "<<plainTape<<" then "<<tape<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-glass-folded.png"),r.body()));
    // Unfolded, the closed face; none clears the outline.
    pressRail(glass->fold);pump();
    CHECK(!glass->isFolded()&&!face->isFolded()&&face->face.isShowing()&&!strip->isShowing()&&!face->isOpen()&&glass->units()==2);
    r.stack->setProposal(nullptr);pump();
    pressRail(glass->fold);pump();
    CHECK(!glass->proposed()&&std::abs(ring()-plainRing)<10);
}
// The lock dots on Glass Current (the Glass Current faceplate spec: The host's parts on a faceplate): AI: KEEP crosses
// the face's left ear while its synth bank is AI-locked; with FACET's and DELAY FDBK's parameters locked, FACET's dot
// shows, and FDBK's only while the open door shows DELAY's first screen. With --images: stack-glass-keep.png.
void stackGlassKeep()
{
    std::cout<<"SECTION stack_glass_keep\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* glass=r.unit(1);auto* face=glass!=nullptr?glass->face():nullptr;
    auto* facet=face!=nullptr?face->face.slider<studio::house::Knob>("facet"):nullptr;auto* drift=face!=nullptr?face->face.slider<studio::house::Knob>("drift"):nullptr;
    auto* fdbk=face!=nullptr?face->revealed().slider<studio::house::Knob>("delay.fdbk"):nullptr;
    auto* delay=face!=nullptr?face->revealed().button("page.delay"):nullptr;auto* filter=face!=nullptr?face->revealed().button("page.filter"):nullptr;
    CHECK(facet!=nullptr&&drift!=nullptr&&fdbk!=nullptr&&delay!=nullptr&&filter!=nullptr);
    if(facet==nullptr||drift==nullptr||fdbk==nullptr||delay==nullptr||filter==nullptr)return;
    const auto k=r.stack->scale();
    const auto yellow=[&](Rect box)
    {
        const auto img=snap(*glass);const auto b=box.getSmallestIntegerContainer().getIntersection(img.getBounds());int n=0;
        for(int y=b.getY();y<b.getBottom();++y)for(int x=b.getX();x<b.getRight();++x)
        {const auto p=img.getPixelAt(x,y);n+=p.getRed()>180&&p.getGreen()>150&&p.getBlue()<120?1:0;}
        return n;
    };
    const Rect ear(0.0f,0.0f,190.0f*k,90.0f*k);
    CHECK(!glass->kept()&&yellow(ear)==0);
    auto d=document(r.p);d.synthLocks[1]=true;CHECK(edit(r.p,d));r.refresh();pump();
    CHECK(glass->kept()&&yellow(ear)>500);
    d=document(r.p);d.controlLocks[facet->element.paramIndex]=true;d.controlLocks[fdbk->element.paramIndex]=true;
    CHECK(edit(r.p,d));r.refresh();pump();
    const auto dot=[&](studio::house::View& v,studio::house::Knob& knob)
    {
        CHECK(knob.element.hasDot);
        return yellow(Rect(18.0f,18.0f).withCentre(glass->getLocalPoint(&v,knob.element.dot)));
    };
    const auto space=juce::KeyPress(juce::KeyPress::spaceKey);
    const auto facetDot=dot(face->face,*facet),driftDot=dot(face->face,*drift),closed=dot(face->revealed(),*fdbk);
    face->setOpen(true);filter->keyPressed(space);pump();
    const auto onFilter=dot(face->revealed(),*fdbk);
    delay->keyPressed(space);pump();
    const auto onDelay=dot(face->revealed(),*fdbk);
    CHECK(facetDot>40&&driftDot<5&&closed<5&&onFilter<5&&onDelay>40);
    std::cout<<"  lock dots: FACET "<<facetDot<<", DRIFT "<<driftDot<<", FDBK closed "<<closed<<", on FILTER "<<onFilter<<", on DELAY "<<onDelay<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-glass-keep.png"),r.body()));
}
// The LIBRARY's preview of Glass Current (the Glass Current faceplate spec: The LIBRARY): its closed faceplate composed at
// the design's values, its VFD reading FACET, 2U, dimmed to half, under the dashes, with its letters on a dark band; the
// units under it move by 2U. The LIBRARY's row for it is library_workflow's. With --images: stack-glass-preview.png.
void stackGlassPreview()
{
    std::cout<<"SECTION stack_glass_preview\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    if(r.divider()==nullptr)return;
    const auto glass=airack::test::frozenExampleRackDesigns()[1];const auto skin=studio::makerFaceFor(glass);
    CHECK(skin!=nullptr);
    if(skin==nullptr)return;
    const auto u=r.stack->unitHeight();const auto dividerTop=r.at(*r.divider()).getY();
    const StackPlacement preview{{RackRole::synth,masterChain,2},"PREVIEW: SYNTH 3, AFTER GLASS CURRENT",rackPreviewInstance(glass),rackPreviewValues(glass)};
    r.stack->setPlacement(preview);pump();
    auto* s=r.stack->placement();
    CHECK(s!=nullptr);
    if(s==nullptr)return;
    const auto k=r.stack->scale();const auto place=r.at(*s);
    CHECK(s->getTitle()==preview.letters&&s->getDescription()=="Glass Current"&&std::abs(place.getHeight()-2.0f*u)<=1.0f
          &&std::abs(r.at(*r.divider()).getY()-(dividerTop+2.0f*u))<=1.0f);
    auto* first=find(*s,"rack.focus.0");
    CHECK(!lettered(*s,"GLASS CURRENT")&&!lettered(*s,preview.letters)&&(first==nullptr||!first->isShowing()));
    const auto composed=studio::MakerFace::composeFor(*skin,*preview.preview,rackPreviewParameters(glass));
    CHECK(composed.getWidth()==2140&&composed.getHeight()==394);
    if(!composed.isValid())return;
    // Away from the dashes and the band: the left ear, the door's top, and the VFD's glass with FACET's line.
    const auto img=r.body();
    for(const auto area:{Rect(40.0f,40.0f,120.0f,60.0f),Rect(1700.0f,90.0f,200.0f,60.0f),Rect(230.0f,140.0f,300.0f,130.0f)})
    {
        const auto shown=meanLuma(img,(area*k).translated((float)studio::HouseUnit::railW*k,place.getY())),drawn=meanLuma(composed,area);
        CHECK(std::abs(shown-(drawn*0.5+2.0))<8.0);
        std::cout<<"  "<<area.toString()<<": shown "<<shown<<", composed "<<drawn<<'\n';
    }
    const auto middle=Rect(773.0f-150.0f,place.getCentreY()-10.0f,300.0f,20.0f).getSmallestIntegerContainer();
    double between=0;int n=0;
    for(int y=middle.getY();y<middle.getBottom();++y)for(int x=middle.getX();x<middle.getRight();++x)
        if(const auto l=luma(img.getPixelAt(x,y));l<60.0){between+=l;++n;}
    CHECK(peakLuma(img,middle.toFloat())>150.0&&n>0&&between/n<25.0);
    if(output!=juce::File{}){r.stack->viewport().setViewPosition(0,(int)place.getY()-200);pump();CHECK(writePng(output.getChildFile("stack-glass-preview.png"),r.body()));}
}
// Oxide Return on the shared master after Thread Press's two effects, its delay in effect bank 6; `twice` places a
// second one after it, its delay in bank 7.
void placeOxide(InstrumentProcessor& p,bool twice=false)
{
    placeRack(p,3,{RackRole::fx,masterChain,2},{{0,5}});
    if(twice)placeRack(p,3,{RackRole::fx,masterChain,3},{{0,6}});
}
// Oxide Return's faceplate in the stack (the delay tape character spec): OXIDE RETURN is a 3U unit showing ARDENTI's
// faceplate between the rails, bound to its own effect bank, a second one to its own, instead of its identity block, UP,
// DOWN and row, its lamp lit while it is IN; Tab runs its triangle, the face's seven knobs, IN / OUT, the tambour and
// OPERATIONS; a press on TIME's bezel, OUT or the tambour never drags, a drag on TIME being one gesture, and one on the
// left ear or the bare plate selects it and a 20 px drag lifts it. With --images: stack-oxide.png.
void stackOxide()
{
    std::cout<<"SECTION stack_oxide\n";
    StackRig r;
    drawnRacks(r.p);placeOxide(r.p,true);r.refresh();pump();
    auto* oxide=r.unit(3);auto* second=r.unit(4);auto* face=oxide!=nullptr?oxide->face():nullptr;
    CHECK(face!=nullptr&&second!=nullptr&&second->face()!=nullptr);
    if(face==nullptr||second==nullptr||second->face()==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();
    CHECK(find(*oxide,"rack.face")==face&&face->isShowing()&&!face->isOpen()&&oxide->units()==3&&std::abs(r.at(*oxide).getHeight()-3.0f*u)<=1.0f);
    CHECK(closeTo(r.at(*face),Rect((float)studio::HouseUnit::railW*k,r.at(*oxide).getY(),2140.0f*k,591.0f*k)));
    CHECK(!lettered(*oxide,"OXIDE RETURN")&&!lettered(*oxide,"SHARED MASTER")&&!oxide->up.isShowing()&&!oxide->down.isShowing()&&!oxide->row.isShowing()
          &&oxide->operations.isShowing());
    CHECK(oxide->name()=="OXIDE RETURN"&&oxide->getTitle()=="Oxide Return");
    const auto d=document(r.p);
    CHECK(oxide->getDescription()==rackHeaderText(d,d.personalities->instances[3])+"\nARDENTI, OXIDE RETURN TAPE ECHO MODEL AR-62");
    auto* time=face->face.slider<studio::house::Knob>("time");auto* in=face->face.lever("in");auto* tambour=face->face.button("tempo");auto* lamp=face->face.lamp("lamp");
    CHECK(time!=nullptr&&in!=nullptr&&tambour!=nullptr&&lamp!=nullptr);
    if(time==nullptr||in==nullptr||tambour==nullptr||lamp==nullptr)return;
    CHECK(time->element.paramIndex==idx("effect.6.delay.seconds")&&in->element.paramIndex==idx("effect.6.bypassed")&&lamp->element.paramIndex==idx("effect.6.bypassed")&&lamp->lit());
    if(auto* note=face->revealed().slider<studio::house::Knob>("note"))CHECK(note->element.paramIndex==idx("effect.6.delay.note"));else CHECK(!"no NOTE");
    if(auto* pp=face->revealed().lever("ping-pong"))CHECK(pp->element.paramIndex==idx("effect.6.delay.ping_pong"));else CHECK(!"no DUAL / PING PONG");
    if(auto* t=second->face()->face.slider<studio::house::Knob>("time"))CHECK(t->element.paramIndex==idx("effect.7.delay.seconds"));else CHECK(!"no second TIME");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-oxide.png"),r.body()));
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(oxide->isParentOf(c))order.add(c->getComponentID());
    const juce::StringArray want{"rack.fold","face.time","face.repeats","face.mix","face.saturation","face.wobble","face.low-cut","face.high-cut","face.in","face.tempo",
                                 "rack.operations"};
    CHECK(order==want);
    if(order!=want)std::cout<<"  Tab order: "<<order.joinIntoString(" ")<<'\n';
    // Thread Press selected and Oxide Return in view; each press 20 px up.
    const auto ids=r.ids();
    Pointer m{*r.stack};
    const auto show=[&]{r.stack->select(ids[2]);r.stack->reveal(ids[3]);pump();};
    const auto at=[&](juce::Component& c,Pt p){return r.stack->getLocalPoint(&c,p);};
    const auto held=[&](Pt from){m.press(from);m.move(from+Pt(0.0f,-10.0f));m.move(from+Pt(0.0f,-20.0f));return r.stack->dragging();};
    {
        // The top of TIME's bezel, beyond a knob's reach about the needle's hub.
        show();
        const auto index=time->element.paramIndex;const auto was=r.p.control(index).getValue();
        HouseRig::Counter gestures;r.p.control(index).addListener(&gestures);
        const auto from=at(*time,local(*time,time->element.hitCentre()-Pt(0.0f,time->element.hitR-12.0f)));
        CHECK(!held(from));
        m.release(from+Pt(0.0f,-20.0f));
        r.p.control(index).removeListener(&gestures);
        CHECK(!r.stack->dragging()&&r.p.control(index).getValue()>was&&r.stack->selected()==ids[3]&&gestures.counts.begins==1&&gestures.counts.ends==1);
    }
    {
        show();
        const auto b=in->element.paramIndex;const auto was=r.p.control(b).getValue();
        const auto from=at(*in,local(*in,in->element.positions[0].legendRect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&r.p.control(b).getValue()!=was&&!lamp->lit());
    }
    {
        show();
        const auto from=at(*tambour,local(*tambour,tambour->element.rect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&face->isOpen());
        face->setOpen(false);pump();
    }
    // The left ear, and the bare plate under REPEATS and TIME.
    for(const auto spot:{Pt(40.0f,300.0f),Pt(850.0f,520.0f)})
    {
        show();juce::Component::unfocusAllComponents();pump();
        const auto from=at(face->face,spot);
        m.press(from);
        CHECK(r.stack->selected()==ids[3]&&!face->hasKeyboardFocus(true));
        m.move(from+Pt(0.0f,-20.0f));
        CHECK(r.stack->dragging());
        m.escape();m.release(m.last);
        CHECK(!r.stack->dragging());
    }
}
// The tambour in the stack (the delay tape character spec): it opens in place, writing nothing, the unit keeping its 3U
// and the space under it where it was; Tab reaches FREE / SYNC, NOTE, DUAL / PING PONG, CROSS, WOBBLE RATE and the pull
// bar only while it is open; closing with focus inside hands it to the tambour; folding to the host's strip closes it, the strip's LED lit
// while the delay is IN, and unfolded it shows closed. With --images: stack-oxide-open.png and stack-oxide-folded.png.
void stackOxideTempo()
{
    std::cout<<"SECTION stack_oxide_tempo\n";
    StackRig r;
    drawnRacks(r.p);placeOxide(r.p);r.refresh();pump();
    auto* oxide=r.unit(3);auto* face=oxide!=nullptr?oxide->face():nullptr;
    auto* tambour=face!=nullptr?face->face.button("tempo"):nullptr;auto* close=face!=nullptr?face->revealed().button("close"):nullptr;
    CHECK(tambour!=nullptr&&close!=nullptr);
    if(tambour==nullptr||close==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();const auto before=r.revisions();const auto spaceTop=r.at(r.space).getY();
    const auto order=[&]{juce::StringArray out;for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(oxide->isParentOf(c))out.add(c->getComponentID());return out;};
    const juce::StringArray closed{"rack.fold","face.time","face.repeats","face.mix","face.saturation","face.wobble","face.low-cut","face.high-cut","face.in","face.tempo",
                                   "rack.operations"};
    const juce::StringArray open{"rack.fold","face.time","face.repeats","face.mix","face.saturation","face.wobble","face.low-cut","face.high-cut","face.in","face.tempo",
                                 "face.sync","face.note","face.ping-pong","face.cross","face.wobble-rate","face.close","rack.operations"};
    CHECK(order()==closed);
    const auto space=juce::KeyPress(juce::KeyPress::spaceKey);
    tambour->keyPressed(space);pump();
    CHECK(face->isOpen()&&face->revealed().isShowing()&&oxide->units()==3&&std::abs(r.at(*oxide).getHeight()-3.0f*u)<=1.0f&&std::abs(r.at(r.space).getY()-spaceTop)<=1.0f
          &&r.revisions()==before);
    CHECK(order()==open);
    if(order()!=open)std::cout<<"  open Tab order: "<<order().joinIntoString(" ")<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-oxide-open.png"),r.body()));
    if(auto* note=face->revealed().slider<studio::house::Knob>("note")){note->grabKeyboardFocus();pump();}else CHECK(!"no NOTE");
    close->keyPressed(space);pump();
    CHECK(!face->isOpen()&&!face->revealed().isShowing()&&tambour->hasKeyboardFocus(false)&&order()==closed&&r.revisions()==before);
    tambour->keyPressed(space);pump();
    pressRail(oxide->fold);pump();
    CHECK(oxide->isFolded()&&oxide->units()==1&&std::abs(r.at(*oxide).getHeight()-u)<=1.0f&&!face->isShowing()&&!face->isOpen()&&!oxide->operations.isShowing()
          &&r.revisions()==before);
    const auto tapeW=std::max(180.0f,studio::house::TapeArt::widthFor(r.model.house,"OXIDE RETURN",54.0f));
    CHECK(meanLuma(snap(*oxide),Rect(8.0f,8.0f).withCentre(Pt((194.0f+tapeW+40.0f)*k,(float)oxide->getHeight()*0.5f)))>150.0);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-oxide-folded.png"),r.body()));
    pressRail(oxide->fold);pump();
    CHECK(!oxide->isFolded()&&oxide->face()==face&&face->isShowing()&&!face->isOpen()&&oxide->units()==3&&oxide->operations.isShowing()&&!oxide->row.isShowing()
          &&order()==closed&&r.revisions()==before);
}
// AI: KEEP and the lock dots on Oxide Return: the tape crosses the faceplate's left ear while its effect bank is
// AI-locked; TIME's dot shows at its place in the skin, REPEATS's not, and NOTE's only while the tambour is open. With
// --images: stack-oxide-keep.png.
void stackOxideKeep()
{
    std::cout<<"SECTION stack_oxide_keep\n";
    StackRig r;
    drawnRacks(r.p);placeOxide(r.p);r.refresh();pump();
    auto* oxide=r.unit(3);auto* face=oxide!=nullptr?oxide->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    const auto k=r.stack->scale();
    const auto yellow=[&](Rect box)
    {
        const auto img=snap(*oxide);const auto b=box.getSmallestIntegerContainer().getIntersection(img.getBounds());int n=0;
        for(int y=b.getY();y<b.getBottom();++y)for(int x=b.getX();x<b.getRight();++x)
        {const auto p=img.getPixelAt(x,y);n+=p.getRed()>180&&p.getGreen()>150&&p.getBlue()<120?1:0;}
        return n;
    };
    const Rect ear(0.0f,0.0f,190.0f*k,90.0f*k);
    CHECK(!oxide->kept()&&yellow(ear)==0);
    auto d=document(r.p);d.effectLocks[5]=true;CHECK(edit(r.p,d));r.refresh();pump();
    CHECK(oxide->kept()&&yellow(ear)>500);
    d=document(r.p);d.controlLocks[idx("effect.6.delay.seconds")]=true;d.controlLocks[idx("effect.6.delay.note")]=true;
    CHECK(edit(r.p,d));r.refresh();pump();
    const auto dot=[&](studio::house::View& v,const char* id)
    {
        auto* knob=v.slider<studio::house::Knob>(id);
        CHECK(knob!=nullptr&&knob->element.hasDot);
        return knob!=nullptr?yellow(Rect(18.0f,18.0f).withCentre(oxide->getLocalPoint(&v,knob->element.dot))):-1;
    };
    const auto time=dot(face->face,"time"),repeats=dot(face->face,"repeats"),closed=dot(face->revealed(),"note");
    face->setOpen(true);pump();
    const auto open=dot(face->revealed(),"note");
    CHECK(time>40&&repeats<5&&closed<5&&open>40);
    std::cout<<"  lock dots: TIME "<<time<<", REPEATS "<<repeats<<", NOTE closed "<<closed<<", open "<<open<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-oxide-keep.png"),r.body()));
}
// The AI's marks on Oxide Return: the stack hands the proposal to its faceplate, not its hidden row; the same candidate
// again draws nothing; folded, the tape has the outline; none clears them. With --images: stack-oxide-marks.png.
void stackOxideMarks()
{
    std::cout<<"SECTION stack_oxide_marks\n";
    StackRig r;
    drawnRacks(r.p);placeOxide(r.p);r.refresh();pump();
    auto* oxide=r.unit(3);auto* face=oxide!=nullptr?oxide->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    auto* time=face->face.slider<studio::house::Knob>("time");auto* in=face->face.lever("in");
    CHECK(time!=nullptr&&in!=nullptr);
    if(time==nullptr||in==nullptr)return;
    const auto t=time->element.paramIndex,b=in->element.paramIndex;
    auto candidate=document(r.p);candidate.values[t]=r.p.control(t).convertTo0to1(.6f);candidate.values[b]=1.0f;
    r.stack->setProposal(&candidate);pump();
    CHECK(face->marks().size()==2&&!face->proposesPanel()&&oxide->proposed()&&oxide->row.marks().empty()&&!oxide->row.proposesPanel());
    CHECK(time->getDescription()=="AI proposes 600 ms"&&in->getDescription()=="AI proposes OUT"&&!oxide->getDescription().contains("AI proposes"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-oxide-marks.png"),r.body()));
    const auto copy=candidate;
    CHECK(paintsOver(*face,[&]{for(int n=0;n<20;++n){r.stack->setProposal(&copy);r.refresh();pump(25);}})==0);
    oxide->fold.onToggle();pump();
    CHECK(oxide->isFolded()&&tapeOutline(*r.stack,*oxide)>=40);
    oxide->fold.onToggle();pump();
    r.stack->setProposal(nullptr);pump();
    CHECK(face->marks().empty()&&!oxide->proposed()&&!oxide->getDescription().contains("AI proposes")&&time->getDescription().isEmpty());
}
RackStackUnit* unitNamed(StackRig& r,const juce::String& name)
{
    for(int n=0;n<8;++n)if(auto* u=r.unit(n);u!=nullptr&&u->name()==name)return u;
    return nullptr;
}
// Night Dial after Glass Current, its wavetable voice in synth bank 3.
RackStackUnit* placeNight(StackRig& r)
{
    placeRack(r.p,5,{RackRole::synth,masterChain,2},{{0,2}});r.refresh();pump();
    auto* night=unitNamed(r,"NIGHT DIAL");
    CHECK(night!=nullptr);
    return night;
}
// Night Dial's faceplate in the stack (the Night Dial spec: The host's parts, Keyboard and accessibility): NIGHT DIAL is a
// 3U unit showing ELMSTEAD SIGNAL WORKS' faceplate between the rails, bound to synth bank 3, instead of its identity
// block, UP, DOWN and row, its backlight lit; Tab runs its triangle, the face's eight knobs, the pull and OPERATIONS; a
// press on DIAL, GLOW or the pull never drags, a drag on a knob being one gesture, and one on the left ear, the bare plate
// or the dial's glass selects it and a 20 px drag lifts it. With --images: stack-night-dial.png.
void stackNight()
{
    std::cout<<"SECTION stack_night\n";
    StackRig r;
    drawnRacks(r.p);auto* night=placeNight(r);auto* face=night!=nullptr?night->face():nullptr;
    CHECK(face!=nullptr&&face->front()==nullptr);
    if(face==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();
    CHECK(find(*night,"rack.face")==face&&face->isShowing()&&!face->isOpen()&&night->units()==3&&std::abs(r.at(*night).getHeight()-3.0f*u)<=1.0f);
    CHECK(closeTo(r.at(*face),Rect((float)studio::HouseUnit::railW*k,r.at(*night).getY(),2140.0f*k,591.0f*k)));
    CHECK(!lettered(*night,"NIGHT DIAL")&&!lettered(*night,"SYNTH 3")&&!night->up.isShowing()&&!night->down.isShowing()&&!night->row.isShowing()
          &&night->operations.isShowing());
    CHECK(night->getTitle()=="Night Dial");
    const auto d=document(r.p);
    CHECK(night->getDescription()==rackHeaderText(d,d.personalities->instances[3])+"\nELMSTEAD SIGNAL WORKS, NIGHT DIAL HYBRID VOICE NDV-2");
    auto* dial=face->face.slider<studio::house::Knob>("dial");auto* glow=face->face.slider<studio::house::Knob>("glow");auto* pull=face->face.button("deck");
    auto* lamp=face->face.lamp("backlight");
    CHECK(dial!=nullptr&&glow!=nullptr&&pull!=nullptr&&lamp!=nullptr);
    if(dial==nullptr||glow==nullptr||pull==nullptr||lamp==nullptr)return;
    CHECK(dial->element.paramIndex==idx("synth.3.wavetable.osc.1.position")&&glow->element.paramIndex==idx("synth.3.wavetable.filter.cutoff")
          &&lamp->element.paramIndex==idx("synth.3.enabled")&&lamp->lit());
    if(auto* noise=face->revealed().slider<studio::house::Knob>("noise"))CHECK(noise->element.paramIndex==idx("synth.3.wavetable.noise"));else CHECK(!"no NOISE LEVEL");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-night-dial.png"),r.body()));
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(night->isParentOf(c))order.add(c->getComponentID());
    const juce::StringArray want{"rack.fold","face.dial","face.sweep","face.spread","face.wander","face.glow","face.ring","face.swell","face.tail","face.deck",
                                 "rack.operations"};
    CHECK(order==want);
    if(order!=want)std::cout<<"  Tab order: "<<order.joinIntoString(" ")<<'\n';
    // Glass Current selected and Night Dial in view; each press 20 px up.
    const auto ids=r.ids();
    Pointer m{*r.stack};
    const auto show=[&]{r.stack->select(ids[1]);r.stack->reveal(ids[3]);pump();};
    const auto at=[&](juce::Component& c,Pt p){return r.stack->getLocalPoint(&c,p);};
    const auto held=[&](Pt from){m.press(from);m.move(from+Pt(0.0f,-10.0f));m.move(from+Pt(0.0f,-20.0f));return r.stack->dragging();};
    for(auto* knob:{dial,glow})
    {
        show();
        const auto index=knob->element.paramIndex;const auto was=r.p.control(index).getValue();
        HouseRig::Counter gestures;r.p.control(index).addListener(&gestures);
        const auto from=at(*knob,local(*knob,knob->element.pivot));
        CHECK(!held(from));
        m.release(from+Pt(0.0f,-20.0f));
        r.p.control(index).removeListener(&gestures);
        CHECK(!r.stack->dragging()&&r.p.control(index).getValue()>was&&r.stack->selected()==ids[3]&&gestures.counts.begins==1&&gestures.counts.ends==1);
    }
    {
        show();
        const auto from=at(*pull,local(*pull,pull->element.rect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&face->isOpen());
        face->setOpen(false);pump();
    }
    // The left ear, the bare plate between SPREAD and WANDER, and the dial's glass, which the needle and the backlight
    // leave to the plate.
    for(const auto spot:{Pt(40.0f,300.0f),Pt(627.0f,300.0f),Pt(1100.0f,180.0f)})
    {
        show();juce::Component::unfocusAllComponents();pump();
        const auto from=at(face->face,spot);
        m.press(from);
        CHECK(r.stack->selected()==ids[3]&&!face->hasKeyboardFocus(true));
        m.move(from+Pt(0.0f,-20.0f));
        CHECK(r.stack->dragging());
        m.escape();m.release(m.last);
        CHECK(!r.stack->dragging());
    }
}
// The deck in the stack (the Night Dial spec: Heights and the reveal): the pull makes the unit 5U and moves the units
// under it down by 2U, writing nothing, scrolling the least that shows the deck; Tab reaches the deck's seventeen
// controls; the pull closes it and the units move back; folding closes it and unfolding shows it closed. With --images:
// stack-night-dial-open.png and stack-night-dial-folded.png.
void stackNightDeck()
{
    std::cout<<"SECTION stack_night_deck\n";
    StackRig r;
    drawnRacks(r.p);auto* night=placeNight(r);auto* face=night!=nullptr?night->face():nullptr;
    auto* pull=face!=nullptr?face->face.button("deck"):nullptr;auto* press=unitNamed(r,"THREAD PRESS");
    CHECK(pull!=nullptr&&press!=nullptr&&r.divider()!=nullptr);
    if(pull==nullptr||press==nullptr||r.divider()==nullptr)return;
    const auto u=r.stack->unitHeight();
    const auto before=r.revisions();
    const auto dividerTop=r.at(*r.divider()).getY(),pressTop=r.at(*press).getY();
    const auto moved=[&](float by){return std::abs(r.at(*r.divider()).getY()-(dividerTop+by))<=1.0f&&std::abs(r.at(*press).getY()-(pressTop+by))<=1.0f;};
    auto& view=r.stack->viewport();
    view.setViewPosition(0,0);pump();
    pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    CHECK(face->isOpen()&&night->units()==5&&std::abs(r.at(*night).getHeight()-5.0f*u)<=1.0f&&moved(2.0f*u)&&r.revisions()==before);
    const auto deck=r.at(face->revealed());
    CHECK(r.inView(face->revealed())&&(view.getViewPositionY()==0||std::abs((float)view.getViewPositionY()-(deck.getBottom()-(float)view.getHeight()))<=1.0f));
    std::cout<<"  open: deck "<<deck.toString()<<", view at "<<view.getViewPositionY()<<'\n';
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(night->isParentOf(c))order.add(c->getComponentID());
    const juce::StringArray want{"rack.fold","face.dial","face.sweep","face.spread","face.wander","face.glow","face.ring","face.swell","face.tail",
        "face.osc1.voices","face.osc1.table","face.osc2.table","face.osc2.position","face.osc2.level","face.osc2.tune","face.osc2.voices","face.osc2.spread",
        "face.width","face.filter.type","face.filter.mode","face.filter.size","face.curve","face.fenv.amount","face.fenv.decay","face.lfo.rate","face.noise",
        "face.deck","rack.operations"};
    CHECK(order==want);
    if(order!=want)std::cout<<"  open Tab order: "<<order.joinIntoString(" ")<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-night-dial-open.png"),r.body()));
    pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    CHECK(!face->isOpen()&&night->units()==3&&moved(0.0f)&&r.revisions()==before);
    // Folding closes it; unfolded, it shows closed.
    pull->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    pressRail(night->fold);pump();
    CHECK(night->isFolded()&&!face->isOpen()&&!face->isShowing()&&night->units()==1&&moved(-2.0f*u)&&r.revisions()==before);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-night-dial-folded.png"),r.body()));
    pressRail(night->fold);pump();
    CHECK(!night->isFolded()&&night->face()==face&&face->isShowing()&&!face->isOpen()&&night->units()==3&&moved(0.0f)&&r.revisions()==before);
}
// AI: KEEP and the lock dots on Night Dial (the Night Dial spec: The host's parts): the tape crosses the faceplate's left
// ear while its wavetable bank is AI-locked; GLOW's dot shows, and OSC 2 SPREAD's only while the deck is open. The
// plate's legends are amber, so each mark is found by what it changes. With --images: stack-night-dial-keep.png.
void stackNightKeep()
{
    std::cout<<"SECTION stack_night_keep\n";
    StackRig r;
    drawnRacks(r.p);auto* night=placeNight(r);auto* face=night!=nullptr?night->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    const auto k=r.stack->scale();
    const auto changed=[](const juce::Image& a,const juce::Image& b,Rect box)
    {
        const auto i=box.getSmallestIntegerContainer().getIntersection(a.getBounds());int n=0;
        for(int y=i.getY();y<i.getBottom();++y)for(int x=i.getX();x<i.getRight();++x)
        {const auto p=a.getPixelAt(x,y),q=b.getPixelAt(x,y);
         n+=std::abs(p.getRed()-q.getRed())>48||std::abs(p.getGreen()-q.getGreen())>48||std::abs(p.getBlue()-q.getBlue())>48?1:0;}
        return n;
    };
    const auto whole=[&]{return Rect(0.0f,0.0f,(float)night->getWidth(),(float)night->getHeight());};
    const Rect ear(0.0f,0.0f,190.0f*k,90.0f*k);
    face->setOpen(true);pump();const auto plainOpen=snap(*night);face->setOpen(false);pump();
    const auto plain=snap(*night);
    CHECK(!night->kept());
    auto d=document(r.p);d.synthLocks[2]=true;CHECK(edit(r.p,d));r.refresh();pump();
    const auto kept=snap(*night);
    CHECK(night->kept()&&changed(kept,plain,ear)>500);
    d=document(r.p);d.controlLocks[idx("synth.3.wavetable.filter.cutoff")]=true;d.controlLocks[idx("synth.3.wavetable.osc.2.unison_cents")]=true;
    CHECK(edit(r.p,d));r.refresh();pump();
    const auto around=[&](studio::house::View& v,const char* id)
    {
        auto* knob=v.slider<studio::house::Knob>(id);
        CHECK(knob!=nullptr&&knob->element.hasDot);
        return knob!=nullptr?Rect(18.0f,18.0f).withCentre(night->getLocalPoint(&v,knob->element.dot)):Rect();
    };
    const auto locked=snap(*night);
    const auto glow=changed(locked,kept,around(face->face,"glow")),ring=changed(locked,kept,around(face->face,"ring"));
    const auto elsewhere=changed(locked,kept,whole())-changed(locked,kept,around(face->face,"glow").expanded(4.0f));
    face->setOpen(true);pump();
    const auto lockedOpen=snap(*night);
    const auto spread=changed(lockedOpen,plainOpen,around(face->revealed(),"osc2.spread")),tune=changed(lockedOpen,plainOpen,around(face->revealed(),"osc2.tune"));
    CHECK(glow>40&&ring<5&&elsewhere<5&&spread>40&&tune<5);
    std::cout<<"  lock dots: GLOW "<<glow<<", RING "<<ring<<", elsewhere closed "<<elsewhere<<", OSC 2 SPREAD open "<<spread<<", OSC 2 TUNE "<<tune<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-night-dial-keep.png"),r.body()));
}
// The AI's marks on Night Dial: the stack hands the proposal to its faceplate, not its hidden row; closed, the deck's
// change waits behind the pull's outline and its description; folded, the tape has the outline; none clears them. With
// --images: stack-night-dial-marks.png and stack-night-dial-marks-open.png.
void stackNightMarks()
{
    std::cout<<"SECTION stack_night_marks\n";
    StackRig r;
    drawnRacks(r.p);auto* night=placeNight(r);auto* face=night!=nullptr?night->face():nullptr;
    auto* pull=face!=nullptr?face->face.button("deck"):nullptr;
    CHECK(pull!=nullptr);
    if(pull==nullptr)return;
    const auto glow=idx("synth.3.wavetable.filter.cutoff"),type=idx("synth.3.wavetable.filter.type");
    auto candidate=document(r.p);candidate.values[glow]=r.p.control(glow).convertTo0to1(3000.0f);candidate.values[type]=r.p.control(type).convertTo0to1(3.0f);
    r.stack->setProposal(&candidate);pump();
    CHECK(face->marks().size()==2&&!face->proposesPanel()&&night->proposed()&&night->row.marks().empty()&&!night->row.proposesPanel());
    CHECK(pull->getDescription()=="AI proposes changes to 1 deck control"&&!night->getDescription().contains("AI proposes"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-night-dial-marks.png"),r.body()));
    face->setOpen(true);pump();
    auto* selector=face->revealed().lever("filter.type");
    CHECK(pull->getDescription().isEmpty()&&selector!=nullptr&&selector->getDescription()=="AI proposes VOWEL");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-night-dial-marks-open.png"),r.body()));
    face->setOpen(false);pump();
    night->fold.onToggle();pump();
    CHECK(night->isFolded()&&tapeOutline(*r.stack,*night)>=40);
    night->fold.onToggle();pump();
    r.stack->setProposal(nullptr);pump();
    CHECK(face->marks().empty()&&!night->proposed()&&pull->getDescription().isEmpty());
}
// The LIBRARY's preview of Night Dial: its closed faceplate composed at the design's values, 3U, dimmed to half, under
// the dashes, with its letters on a dark band; the units under it move by 3U. With --images: stack-night-dial-preview.png.
void stackNightPreview()
{
    std::cout<<"SECTION stack_night_preview\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    if(r.divider()==nullptr)return;
    const auto night=airack::test::frozenExampleRackDesigns()[5];const auto skin=studio::makerFaceFor(night);
    CHECK(skin!=nullptr);
    if(skin==nullptr)return;
    const auto u=r.stack->unitHeight();const auto dividerTop=r.at(*r.divider()).getY();
    const StackPlacement preview{{RackRole::synth,masterChain,2},"PREVIEW: SYNTH 3, AFTER GLASS CURRENT",rackPreviewInstance(night),rackPreviewValues(night)};
    r.stack->setPlacement(preview);pump();
    auto* s=r.stack->placement();
    CHECK(s!=nullptr);
    if(s==nullptr)return;
    const auto k=r.stack->scale();const auto place=r.at(*s);
    CHECK(s->getTitle()==preview.letters&&s->getDescription()=="Night Dial"&&std::abs(place.getHeight()-3.0f*u)<=1.0f
          &&std::abs(r.at(*r.divider()).getY()-(dividerTop+3.0f*u))<=1.0f);
    auto* first=find(*s,"rack.focus.0");
    CHECK(!lettered(*s,"NIGHT DIAL")&&!lettered(*s,preview.letters)&&(first==nullptr||!first->isShowing()));
    const auto composed=studio::MakerFace::composeFor(*skin,*preview.preview,rackPreviewParameters(night));
    CHECK(composed.getWidth()==2140&&composed.getHeight()==591);
    if(!composed.isValid())return;
    const auto img=r.body();
    // The glass, DIAL and the shutter's housing.
    for(const auto area:{Rect(300.0f,90.0f,150.0f,40.0f),Rect(1780.0f,70.0f,140.0f,40.0f),Rect(300.0f,490.0f,200.0f,40.0f)})
    {
        const auto shown=meanLuma(img,(area*k).translated((float)studio::HouseUnit::railW*k,place.getY())),drawn=meanLuma(composed,area);
        CHECK(std::abs(shown-(drawn*0.5+2.0))<8.0);
        std::cout<<"  "<<area.toString()<<": shown "<<shown<<", composed "<<drawn<<'\n';
    }
    const auto middle=Rect(773.0f-150.0f,place.getCentreY()-10.0f,300.0f,20.0f).getSmallestIntegerContainer();
    double between=0;int n=0;
    for(int y=middle.getY();y<middle.getBottom();++y)for(int x=middle.getX();x<middle.getRight();++x)
        if(const auto l=luma(img.getPixelAt(x,y));l<60.0){between+=l;++n;}
    CHECK(peakLuma(img,middle.toFloat())>150.0&&n>0&&between/n<25.0);
    if(output!=juce::File{}){r.stack->viewport().setViewPosition(0,(int)place.getY()-200);pump();CHECK(writePng(output.getChildFile("stack-night-dial-preview.png"),r.body()));}
}
// The motion's sections stand its clock still and step it, with reduced motion off; main forces it on for the rest.
struct MotionClock
{
    double t=0;
    MotionClock() { studio::motion::setForTests([this]{return t;},false); }
    ~MotionClock() { studio::motion::setForTests({},true); }
    MotionClock(const MotionClock&)=delete;
    void at(double ms) { t=ms;pump();juce::Timer::callPendingTimersSynchronously(); }
};
// The stack follows the faceplates' drawn heights (the faceplate motion spec, decision 9): EMBER REED's drawer moves
// the units below it by fractions of a unit, the view keeping the opening in view on each frame and landing where the
// jump puts it; GLASS CURRENT's door moves THREAD PRESS; THREAD PRESS's cover moves nothing.
void stackMotion()
{
    std::cout<<"SECTION stack_motion\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* glass=r.unit(1);auto* press=r.unit(2);
    auto* face=ember!=nullptr?ember->face():nullptr;auto* glassFace=glass!=nullptr?glass->face():nullptr;auto* pressFace=press!=nullptr?press->face():nullptr;
    auto* pull=face!=nullptr&&face->front()!=nullptr?face->front()->button("programmer"):nullptr;
    auto* edit=glassFace!=nullptr?glassFace->face.button("edit"):nullptr;
    auto* close=glassFace!=nullptr&&glassFace->hanging()!=nullptr?glassFace->hanging()->button("close"):nullptr;
    auto* cover=pressFace!=nullptr?pressFace->face.button("setup"):nullptr;
    CHECK(pull!=nullptr&&edit!=nullptr&&close!=nullptr&&cover!=nullptr&&r.divider()!=nullptr);
    if(pull==nullptr||edit==nullptr||close==nullptr||cover==nullptr||r.divider()==nullptr)return;
    const auto u=r.stack->unitHeight();
    const auto glassTop=r.at(*glass).getY(),dividerTop=r.at(*r.divider()).getY(),pressTop=r.at(*press).getY();
    const auto below=[&](float by){return std::abs(r.at(*r.divider()).getY()-(dividerTop+by))<=1.0f&&std::abs(r.at(*press).getY()-(pressTop+by))<=1.0f;};
    const auto moved=[&](float by){return std::abs(r.at(*glass).getY()-(glassTop+by))<=1.0f&&below(by);};
    const auto space=juce::KeyPress(juce::KeyPress::spaceKey);
    auto& view=r.stack->viewport();
    view.setViewPosition(0,0);pump();
    MotionClock clock;
    pull->keyPressed(space);
    clock.at(100);
    CHECK(face->isMoving()&&moved(2.0f*u)&&std::abs(r.at(*ember).getHeight()-5.0f*u)<=1.0f);
    clock.at(150);const auto midway=view.getViewPositionY();
    clock.at(200);
    const auto shown=r.at(face->revealed()).getUnion(r.at(*face->front()));
    CHECK(!face->isMoving()&&face->isOpen()&&moved(4.0f*u)&&std::abs(r.at(*ember).getHeight()-7.0f*u)<=1.0f
          &&std::abs((float)view.getViewPositionY()-(shown.getBottom()-(float)view.getHeight()))<=1.0f&&midway>0&&midway<=view.getViewPositionY());
    std::cout<<"  drawer: the view at "<<midway<<" at 150 ms, "<<view.getViewPositionY()<<" open\n";
    clock.at(300);pull->keyPressed(space);clock.at(500);
    CHECK(!face->isMoving()&&!face->isOpen()&&moved(0.0f));
    clock.at(600);edit->keyPressed(space);
    clock.at(690);CHECK(glassFace->isMoving()&&below(u)&&std::abs(r.at(*glass).getY()-glassTop)<=1.0f);
    clock.at(780);CHECK(!glassFace->isMoving()&&glassFace->isOpen()&&below(2.0f*u));
    close->keyPressed(space);clock.at(960);
    CHECK(!glassFace->isMoving()&&!glassFace->isOpen()&&moved(0.0f));
    clock.at(1000);static_cast<juce::Component*>(cover)->keyPressed(space);
    clock.at(1110);CHECK(pressFace->isMoving()&&moved(0.0f)&&std::abs(r.at(*press).getHeight()-2.0f*u)<=1.0f);
    clock.at(1220);CHECK(!pressFace->isMoving()&&pressFace->isOpen()&&moved(0.0f));
}
// Review Focus 5: a rack that the column folds while its door moves closes at once, and the stack lands on whole units.
void stackMotionFold()
{
    std::cout<<"SECTION stack_motion_fold\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* glass=r.unit(1);auto* face=glass!=nullptr?glass->face():nullptr;auto* edit=face!=nullptr?face->face.button("edit"):nullptr;
    CHECK(edit!=nullptr);
    if(edit==nullptr)return;
    const auto ids=r.ids();const auto u=r.stack->unitHeight();
    r.stack->setSize(1143,754);pump();
    CHECK(r.stack->narrow()&&r.stack->selected()==ids[0]);
    r.stack->select(ids[1]);pump();
    CHECK(!glass->isFolded());
    MotionClock clock;
    edit->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    clock.at(90);CHECK(face->isOpen()&&face->isMoving());
    r.stack->select(ids[0]);pump();
    CHECK(glass->isFolded()&&!face->isOpen()&&!face->isMoving()&&face->opening()==0.0f);
    bool whole=true;
    for(auto* x:r.stack->units()){const auto y=r.at(*x).getY();whole=whole&&std::abs(y-std::round(y/u)*u)<=1.0f;}
    CHECK(whole);
}
// Review Focus 2: the rack retired while its door moves, as the host recalls the state from before its placement: no
// callback reaches the deleted faceplate, and the stack's last unit is EMBER REED's 3U.
void stackMotionRetire()
{
    std::cout<<"SECTION stack_motion_retire\n";
    StackRig r;
    placeRack(r.p,0,{},{{0,0},{1,0}},true);
    juce::MemoryBlock before;r.p.getStateInformation(before);
    placeRack(r.p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}});
    r.refresh();pump();
    auto* glass=r.unit(1);auto* face=glass!=nullptr?glass->face():nullptr;auto* edit=face!=nullptr?face->face.button("edit"):nullptr;
    CHECK(edit!=nullptr);
    if(edit==nullptr)return;
    MotionClock clock;
    edit->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    clock.at(90);CHECK(face->isMoving());
    r.p.setStateInformation(before.getData(),(int)before.getSize());
    CHECK(wait("Glass Current retired",[&]{r.refresh();return r.unit(1)==nullptr;}));
    clock.at(400);pump();pump();
    auto* ember=r.unit(0);const auto u=r.stack->unitHeight();
    CHECK(ember!=nullptr&&r.stack->units().size()==1&&ember->units()==3&&std::abs(r.at(*ember).getHeight()-3.0f*u)<=1.0f);
}
// Frames side by side, each scaled by `scale`.
juce::Image filmstrip(const std::vector<juce::Image>& frames,float scale)
{
    if(frames.empty())return {};
    const auto w=juce::roundToInt((float)frames[0].getWidth()*scale),h=juce::roundToInt((float)frames[0].getHeight()*scale);
    juce::Image out(juce::Image::ARGB,w*(int)frames.size(),h,true);juce::Graphics g(out);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    for(std::size_t i=0;i<frames.size();++i)g.drawImageTransformed(frames[i],juce::AffineTransform::scale(scale).translated((float)((int)i*w),0.0f));
    return out;
}
// With --images, each reveal in the stack at 0, 25, 50, 75 and 100 % of its time, side by side: motion-thread-press.png
// (the cover and 40 face px round it), motion-ember-reed.png (the unit open and 1U below, at half size) and
// motion-glass-current.png (the door's panel and the door, and 40 face px round them).
void stackMotionSnapshots()
{
    std::cout<<"SECTION stack_motion_snapshots\n";
    if(output==juce::File{})return;
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    auto* ember=r.unit(0);auto* face=ember!=nullptr?ember->face():nullptr;auto* glass=r.unit(1)!=nullptr?r.unit(1)->face():nullptr;
    auto* press=r.unit(2)!=nullptr?r.unit(2)->face():nullptr;
    auto* pull=face!=nullptr&&face->front()!=nullptr?face->front()->button("programmer"):nullptr;
    auto* edit=glass!=nullptr?glass->face.button("edit"):nullptr;auto* cover=press!=nullptr?press->face.button("setup"):nullptr;
    CHECK(pull!=nullptr&&edit!=nullptr&&cover!=nullptr);
    if(pull==nullptr||edit==nullptr||cover==nullptr)return;
    const auto u=r.stack->unitHeight();
    const auto film=[&](const char* name,studio::MakerFace& f,juce::Component& opener,int ms,Rect region,float scale)
    {
        std::vector<juce::Image> frames;
        {
            MotionClock clock;
            opener.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
            for(int k=0;k<=4;++k){clock.at(ms*k/4.0);frames.push_back(r.body().getClippedImage(region.getSmallestIntegerContainer()));}
        }
        CHECK(!f.isMoving()&&f.isOpen()&&writePng(output.getChildFile(name),filmstrip(frames,scale)));
        f.setOpen(false,false);pump();
    };
    const auto lid=press->skin.skin.reveal.motion.lid.expanded(40.0f);
    film("motion-thread-press.png",*press,*cover,220,r.stack->body().getLocalArea(press,lid),1.0f);
    const auto unit=r.at(*ember);
    film("motion-ember-reed.png",*face,*pull,200,unit.withHeight(8.0f*u),0.5f);
    const auto plate=[&](const juce::String& view){const auto& a=glass->face.ctx.model.asset(view+":plate");return Rect(a.anchor.x,a.anchor.y,(float)a.image.getWidth(),(float)a.image.getHeight());};
    const auto swung=plate(glass->revealed().name).getUnion(plate("door"));
    film("motion-glass-current.png",*glass,*edit,180,r.stack->body().getLocalArea(glass,swung.expanded(40.0f)),1.0f);
}
// Review Focus 2: the editor closed by a listener as a rack unit's control begins its gesture. The unit's row keeps the
// editor until the callback frame unwinds; then it goes, ending the gesture.
void windowUnitRetires()
{
    std::cout<<"SECTION window_unit_retires\n";
    WindowRig w;
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    auto* unit=w.rack(0);
    auto* slider=unit!=nullptr?dynamic_cast<juce::Slider*>(find(*unit,"rack.focus.0")):nullptr;
    const auto index=firstControl(document(w.p),0);
    CHECK(slider!=nullptr&&index.has_value());
    if(slider==nullptr||!index)return;
    juce::Component::SafePointer<juce::Slider> deferred(slider);
    struct Closer final:juce::AudioProcessorParameter::Listener
    {
        std::function<void()> onBegin;
        void parameterValueChanged(int,float) override {}
        void parameterGestureChanged(int,bool starting) override { if(starting&&onBegin)onBegin(); }
    } closer;
    bool inside=false;
    closer.onBegin=[&]{w.editor.reset();inside=deferred!=nullptr;};
    w.p.control(*index).addListener(&closer);
    slider->onDragStart();
    w.p.control(*index).removeListener(&closer);
    CHECK(inside&&deferred!=nullptr);
    pump();
    CHECK(deferred==nullptr&&w.p.beginParameterGesture(*index)&&w.p.endParameterGesture(*index));
}
// A recalled face mismatch queues its old controls for deletion. Closing the editor before that turn must also
// release a cancelled drag once, without reaching back into the deleted editor's callback-frame owner.
void makerFaceRetires(bool dragging,bool removed)
{
    std::cout<<"SECTION window_maker_face_retires dragging="<<dragging<<" removed="<<removed<<std::endl;
    WindowRig w;auto* stack=w.stack();CHECK(stack);if(!stack)return;
    const auto empty=document(w.p);
    const auto factories=airack::test::frozenFactoryRackDesigns();
    const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Band Color";});
    CHECK(found!=factories.end());if(found==factories.end())return;
    const auto id=placeFactory(w.p,std::size_t(found-factories.begin()),{RackRole::fx,masterChain,0},{{0,6}});
    CHECK(wait("Band Color before retirement",[&]{return stack->unit(id)&&stack->unit(id)->face();}));
    auto* unit=stack->unit(id);auto* face=unit?unit->face():nullptr;CHECK(face);if(!face)return;
    auto* knob=face->face.slider<studio::house::Knob>("mid-drive");CHECK(knob);if(!knob)return;
    juce::Component::SafePointer<studio::MakerFace> retired(face);
    const auto before=document(w.p);RackOperation change;change.kind=RackOperationKind::face;change.selected=id;
    for(const auto& rack:before.personalities->instances)if(rack.id==id)change.design=rack.design;
    CHECK(change.design.has_value());if(!change.design)return;change.design->panel.controls[0].label="Changed";
    SoundDocument recalled;RackDifference difference;
    CHECK(makeRackCandidate(before,change,SoundEditOrigin::manual,recalled,difference)==RackCandidateStatus::ready);
    if(removed)recalled=empty;
    juce::MemoryBlock bytes;CHECK(encodeSoundState(recalled,bytes));
    const auto index=*rackParameterIndex(NativeModule::saturation,6,"saturation.mid.drive_db");
    HouseRig::Counter gestures;w.p.control(index).addListener(&gestures);
    if(dragging)knob->onDragStart();
    CHECK(gestures.counts.begins==(dragging?1:0)&&gestures.counts.ends==0);
    w.p.setStateInformation(bytes.getData(),(int)bytes.getSize());
    stack->refresh(document(w.p));
    CHECK((removed?stack->unit(id)==nullptr:stack->unit(id)&&!stack->unit(id)->face())&&retired!=nullptr);
    w.editor.reset();
    pump();
    w.p.control(index).removeListener(&gestures);
    CHECK(retired==nullptr&&document(w.p)==recalled);
    CHECK(gestures.counts.begins==(dragging?1:0)&&gestures.counts.ends==(dragging?1:0));
    CHECK(w.p.beginParameterGesture(index)&&w.p.endParameterGesture(index));
}
void windowMakerFaceRetiresIdle(){makerFaceRetires(false,false);}
void windowMakerFaceRetiresDragging(){makerFaceRetires(true,false);}
void windowMakerUnitRetiresIdle(){makerFaceRetires(false,true);}
void windowMakerUnitRetiresDragging(){makerFaceRetires(true,true);}
// Review Focus 4, the rest: ADVANCED hides the drawer with the stack; RACK brings it back under its rack with the
// chosen operation kept; beside the AI unit it is scaled down with the stack.
void windowDrawerViews()
{
    std::cout<<"SECTION window_drawer_views\n";
    WindowRig w;
    auto& e=*w.editor;
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    auto* unit=w.rack(0);
    auto* drawer=w.drawer();
    auto* stage=w.stage();
    auto* lever=widget<studio::Lever>(e,"rack.toggle");
    CHECK(unit!=nullptr&&drawer!=nullptr);
    if(unit==nullptr||drawer==nullptr||stage==nullptr||lever==nullptr)return;
    auto* operation=widget<juce::ComboBox>(*drawer,"rack.operation");
    auto* apply=widget<juce::Button>(*drawer,"rack.apply");
    if(operation==nullptr||apply==nullptr)return;
    click(unit->operations);
    operation->setSelectedId(5,juce::sendNotificationSync);
    CHECK(drawer->isShowing()&&apply->isEnabled());
    // Under its rack: the all-controls panel on the rack's bottom, the form on the panel's.
    auto* panel=widget<juce::Component>(e,"rack.controls");
    if(panel==nullptr)return;
    const auto under=[&]{return drawer->isShowing()&&std::abs(w.box(*panel).getY()-w.box(*unit).getBottom())<=1.0f&&std::abs(w.box(*drawer).getY()-w.box(*panel).getBottom())<=1.0f;};
    click(*drawer,"rack.advanced");
    CHECK(w.showing("pages")&&!drawer->isShowing());
    click(*lever);
    CHECK(under()&&apply->isEnabled());
    // Beside the AI unit the stack is scaled to the column: the drawer spans the stack between the rails, 31 to 1516,
    // scaled by 1143 / 1547.
    click(e,"ai.toggle");
    const auto s=stage->getTransform().mat00;
    CHECK(w.showing("ai.panel")&&under()&&apply->isEnabled()&&std::abs(w.box(*drawer).getWidth()-1485.0f*1143.0f/1547.0f*s)<=1.0f);
    if(!under()||std::abs(w.box(*drawer).getWidth()-1485.0f*1143.0f/1547.0f*s)>1.0f)std::cout<<"  drawer beside the AI unit: "<<w.box(*drawer).toString()<<'\n';
}
// The editor refreshes the stack on its 20 ms timer; while the sound is still, nothing over a rack's unit is drawn again.
void windowStackIdles()
{
    std::cout<<"SECTION window_stack_idles\n";
    WindowRig w;
    placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));
    auto* stack=w.stack();
    auto* unit=w.rack(0);
    CHECK(stack!=nullptr&&unit!=nullptr);
    if(stack==nullptr||unit==nullptr)return;
    struct Counter final:juce::Component { int paints=0;void paint(juce::Graphics&) override { ++paints; } } counter;
    counter.setInterceptsMouseClicks(false,false);
    stack->body().addAndMakeVisible(counter);counter.setBounds(unit->getBounds());
    pump(200);counter.paints=0;
    pump(500);
    const int paints=counter.paints;
    stack->body().removeChildComponent(&counter);
    CHECK(paints==0);
    std::cout<<"  paints over the unit in 500 ms: "<<paints<<'\n';
}
// Gallery Cancel restores selection/scroll; explicit Add closes and selects the new rack.
void windowLibraryPreview()
{
    std::cout<<"SECTION window_library_preview\n";WindowRig w;drawnRacks(w.p);CHECK(wait("drawn racks",[&]{return w.rack(2)!=nullptr;}));auto* stack=w.stack();if(!stack)return;auto& e=*w.editor;
    stack->select(w.rack(1)->id());stack->viewport().setViewPosition(0,100);const auto selected=stack->selected();const auto scroll=stack->viewport().getViewPosition();const auto before=document(w.p);click(e,"rack.add");CHECK(chooseLibrary(e,"Copper Arc"));CHECK(!stack->isShowing()&&!stack->placement()&&document(w.p)==before);click(e,"library.cancel");CHECK(stack->isShowing()&&stack->selected()==selected&&stack->viewport().getViewPosition()==scroll&&document(w.p)==before);
    e.setSize(1600,1000);click(e,"rack.add");CHECK(chooseLibrary(e,"Copper Arc"));click(e,"library.apply");CHECK(wait("placed rack selected",[&]{return !w.inLibrary()&&stack->unit(stack->selected())&&stack->unit(stack->selected())->name()=="COPPER ARC";}));
}
// The full-workspace Gallery hides the stack; background rack paint and sound remain idle while browsing.
void windowLibraryStackIdles()
{
    std::cout<<"SECTION window_library_stack_idles\n";WindowRig w;auto& e=*w.editor;placeRack(w.p,0,{},{{0,0},{1,0}},true);
    CHECK(wait("the placed rack's unit",[&]{return w.showing("rack.unit.0");}));auto* stack=w.stack();auto* unit=w.rack(0);CHECK(stack&&unit);if(!stack||!unit)return;
    const auto revisions=w.revisions();click(e,"rack.add");CHECK(w.inLibrary()&&!stack->isShowing()&&!stack->placement());pump(200);CHECK(paintsOver(*unit,[]{pump(500);})==0);CHECK(w.revisions()==revisions);
    click(e,"library.cancel");CHECK(stack->isShowing()&&unit->isShowing()&&w.revisions()==revisions);
}
// Catch clipping or stale transforms/bindings when a calibrated face shares the full editor with either side unit.
void stackBandColorViewportMatrix(WindowRig& w,RackStack& stack,RackStackUnit& unit,studio::MakerFace& face)
{
    std::cout<<"SECTION stack_band_color_viewport_matrix snapshot_bytes="<<sizeof(SoundDocument)<<"\n";
    auto& editor=*w.editor;
    auto* cover=face.face.button("calibration");auto* close=face.revealed().button("close");
    CHECK(cover&&close);if(!cover||!close)return;
    const auto mode=*rackParameterIndex(NativeModule::saturation,6,"saturation.mode");
    const auto initial=document(w.p);
    const std::pair<const char*,const char*> bindings[]{
        {"low-crossover","saturation.low_crossover_hz"},{"high-crossover","saturation.high_crossover_hz"},
        {"mid-drive","saturation.mid.drive_db"},{"mid-mix","saturation.mid.mix"},
        {"high-drive","saturation.high.drive_db"},{"high-mix","saturation.high.mix"},
        {"high-tilt","saturation.high.pre_tilt_db"},{"global-mix","saturation.mix"}};
    const auto hit=[&](studio::house::View& view,juce::Component& control,Pt drawn)
    {
        const auto at=editor.getLocalPoint(&view,drawn);
        CHECK(control.isShowing()&&editor.getLocalBounds().toFloat().contains(at));
        CHECK(editor.getComponentAt(at.roundToInt())==&control);
    };
    for(const auto size:{juce::Point<int>(1080,680),juce::Point<int>(1547,965)})
    {
        editor.setSize(size.x,size.y);pump(120);
        for(const auto* panel:{"none","ai","library"})
        {
            if(juce::String(panel)=="ai")click(editor,"ai.toggle");
            else if(juce::String(panel)=="library")click(editor,"rack.add");
            CHECK(w.showing("ai.panel")==(juce::String(panel)=="ai")&&w.inLibrary()==(juce::String(panel)=="library"));
            if(juce::String(panel)=="library"){
                CHECK(!stack.isShowing()&&!face.isShowing());
                auto* library=find(editor,"library.panel");CHECK(library&&library->isShowing());
                auto preserved=std::make_unique<SoundDocument>(),after=std::make_unique<SoundDocument>();std::uint64_t edit;CHECK(w.p.trySoundSnapshot(*preserved,edit));const auto saved=w.revisions();
                click(editor,"library.cancel");CHECK(w.p.trySoundSnapshot(*after,edit));CHECK(!w.inLibrary()&&stack.isShowing()&&*after==*preserved&&w.revisions()==saved);
                continue;
            }
            const auto unchanged=document(w.p);const auto revisions=w.revisions();
            for(const bool open:{false,true})
            {
                if(face.isOpen())CHECK(close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
                if(open)CHECK(cover->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
                pump();stack.viewport().setViewPosition(0,std::max(0,unit.getY()-8));pump(60);
                const auto name="band-color-context-"+juce::String(size.x)+"-"+panel+(open?"-open":"-closed");
                std::cout<<"  CONTEXT "<<name<<'\n';
                const auto column=w.box(stack.viewport()),rack=w.box(unit),plate=w.box(face);
                CHECK(editor.getWidth()==size.x&&editor.getHeight()==size.y&&face.isOpen()==open&&face.getHeight()==394);
                CHECK(!unit.isFolded()&&!unit.columnFolded()&&unit.across()==0&&unit.shift()==0&&!stack.narrow());
                CHECK(rack.getX()>=column.getX()-1&&rack.getRight()<=column.getRight()+1
                      &&plate.getX()>=column.getX()-1&&plate.getRight()<=column.getRight()+1);
                CHECK(stack.viewport().getViewPositionX()==0&&!stack.viewport().getHorizontalScrollBar().isVisible());
                CHECK(stack.drawerRack().isNull()&&document(w.p)==unchanged&&w.revisions()==revisions);
                for(const auto& [localId,key]:bindings)
                {
                    auto* knob=face.face.slider<studio::house::Knob>(localId);CHECK(knob);if(!knob)continue;
                    const auto index=*rackParameterIndex(NativeModule::saturation,6,key);
                    CHECK(knob->element.paramIndex==index&& &knob->param==&w.p.control(index));
                    CHECK(knob->isEnabled()&&!knob->getDescription().containsIgnoreCase("inactive")
                          &&std::abs(knob->getValue()-parameterPhysicalValue(unchanged.values,index))<1.e-4);
                    hit(face.face,*knob,knob->element.pivot);
                }
                CHECK(face.revealed().isShowing()==open);
                if(open)
                {
                    for(const auto& [localId,key]:{std::pair{"input-trim","saturation.input_trim_db"},std::pair{"output-trim","saturation.output_trim_db"}})
                    {
                        auto* knob=face.revealed().slider<studio::house::Knob>(localId);CHECK(knob);if(!knob)continue;
                        const auto index=*rackParameterIndex(NativeModule::saturation,6,key);
                        CHECK(knob->element.paramIndex==index&& &knob->param==&w.p.control(index)
                              &&std::abs(knob->getValue()-parameterPhysicalValue(unchanged.values,index))<1.e-4);
                        hit(face.revealed(),*knob,knob->element.pivot);
                    }
                    for(const auto& [localId,key]:{std::pair{"mode","saturation.mode"},std::pair{"compensation","saturation.drive_compensation"}})
                        if(auto* lever=face.revealed().lever(localId))
                        {
                            CHECK(lever->element.paramIndex==*rackParameterIndex(NativeModule::saturation,6,key));
                            hit(face.revealed(),*lever,lever->element.pivot);
                        }
                        else CHECK(false);
                    hit(face.revealed(),*close,close->element.rect.getCentre());
                }
                else hit(face.face,*cover,cover->element.rect.getCentre());
                if(output!=juce::File{})CHECK(writePng(output.getChildFile(name+".png"),snap(editor)));
            }
            // Full mode retains editable band controls through every size/panel transition; a key edits only its bound bank.
            auto full=unchanged;full.values[mode]=0;CHECK(edit(w.p,full));pump();
            for(const auto& [localId,key]:bindings)
            {
                auto* knob=face.face.slider<studio::house::Knob>(localId);if(!knob)continue;
                CHECK(knob->isEnabled()&&knob->getDescription().containsIgnoreCase("inactive")== (juce::String(key)!="saturation.mix"));
            }
            auto* drive=face.face.slider<studio::house::Knob>("mid-drive");
            if(drive)
            {
                CHECK(drive->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));pump();
                auto expected=full;const auto mid=*rackParameterIndex(NativeModule::saturation,6,"saturation.mid.drive_db");
                const auto changed=document(w.p);CHECK(changed.values[mid]!=full.values[mid]);expected.values[mid]=changed.values[mid];CHECK(changed==expected);
                CHECK(w.p.undoSoundEdit());pump();CHECK(document(w.p)==full);
            }
            CHECK(w.p.undoSoundEdit());pump();CHECK(document(w.p)==unchanged);
        }
        if(w.inLibrary())click(editor,"library.cancel");if(w.showing("ai.panel"))click(editor,"ai.toggle");pump();CHECK(!w.inLibrary()&&!w.showing("ai.panel"));
    }
    // Select Full once: resizing with AI/Library open must not reset mode, bindings or retained-bank relevance.
    std::cout<<"SECTION stack_band_color_already_full\n";
    auto full=initial;full.values[mode]=0;CHECK(edit(w.p,full));pump();
    const auto fullRevisions=w.revisions();const auto coverHeight=face.getHeight();
    const std::pair<juce::Point<int>,const char*> transitions[]{
        {{1080,680},"none"},{{1080,680},"ai"},{{1547,965},"ai"},{{1547,965},"library"},
        {{1080,680},"library"},{{1080,680},"none"},{{1547,965},"none"}};
    for(const auto& [size,panel]:transitions)
    {
        editor.setSize(size.x,size.y);pump(120);
        if(juce::String(panel)=="none")
        {
            if(w.inLibrary())click(editor,"library.cancel");
            if(w.showing("ai.panel"))click(editor,"ai.toggle");
        }
        else if(juce::String(panel)=="ai"&&!w.showing("ai.panel"))click(editor,"ai.toggle");
        else if(juce::String(panel)=="library"&&!w.inLibrary())click(editor,"rack.add");
        stack.viewport().setViewPosition(0,std::max(0,unit.getY()-8));pump(60);
        std::cout<<"  FULL_CONTEXT band-color-full-"<<size.x<<'-'<<panel<<'\n';
        CHECK(document(w.p).values[mode]==0&&document(w.p)==full&&w.revisions()==fullRevisions);
        CHECK(editor.getWidth()==size.x&&editor.getHeight()==size.y&&face.isOpen()
              &&coverHeight==394&&face.getHeight()==coverHeight&&stack.drawerRack().isNull());
        CHECK(w.showing("ai.panel")==(juce::String(panel)=="ai")&&w.inLibrary()==(juce::String(panel)=="library"));
        if(juce::String(panel)=="library"){CHECK(!stack.isShowing()&&!face.isShowing());continue;}
        for(const auto& [localId,key]:bindings)
        {
            auto* knob=face.face.slider<studio::house::Knob>(localId);CHECK(knob);if(!knob)continue;
            const auto index=*rackParameterIndex(NativeModule::saturation,6,key);
            CHECK(knob->element.paramIndex==index&& &knob->param==&w.p.control(index)
                  &&std::abs(knob->getValue()-parameterPhysicalValue(full.values,index))<1.e-4);
            CHECK(knob->isEnabled()&&knob->getDescription().containsIgnoreCase("inactive")== (juce::String(key)!="saturation.mix"));
            hit(face.face,*knob,knob->element.pivot);
        }
        hit(face.revealed(),*close,close->element.rect.getCentre());
        if(output!=juce::File{}&&((size.x==1547&&juce::String(panel)=="ai")||(size.x==1080&&juce::String(panel)=="library")))
            CHECK(writePng(output.getChildFile("band-color-full-"+juce::String(size.x)+"-"+panel+".png"),snap(editor)));
    }
    CHECK(w.p.undoSoundEdit());pump();CHECK(document(w.p)==initial);
    CHECK(close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));pump();
    CHECK(!face.isOpen()&&document(w.p)==initial&&stack.drawerRack().isNull());
}
void stackBandColor()
{
    std::cout<<"SECTION stack_band_color\n";
    auto storage=std::make_unique<WindowRig>();auto& w=*storage;auto* stack=w.stack();CHECK(stack);if(!stack)return;
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bf02");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    const auto id=placeFactory(w.p,std::size_t(found-factories.begin()),{RackRole::fx,masterChain,0},{{0,6}});
    CHECK(wait("Band Color equipment in actual editor",[&]{return stack->unit(id)&&stack->unit(id)->face();}));
    auto* unit=stack->unit(id);if(!unit||!unit->face())return;auto* face=unit->face();
    CHECK(face->skin.maker=="VELDRUM AUDIO"&&face->skin.model=="BAND COLOR"&&!unit->on()&&unit->getDescription().contains(" (bypassed)"));
    const auto shot=[&](const char* name){pump(80);if(output!=juce::File{})CHECK(writePng(output.getChildFile(name),snap(*w.editor)));};
    w.editor->setSize(1080,680);pump(120);CHECK(unit->getWidth()<=stack->body().getWidth());shot("band-color-editor-1080-closed.png");
    const auto mid=*rackParameterIndex(NativeModule::saturation,6,"saturation.mid.drive_db"),high=*rackParameterIndex(NativeModule::saturation,6,"saturation.high.drive_db"),tilt=*rackParameterIndex(NativeModule::saturation,6,"saturation.high.pre_tilt_db");
    CHECK(std::abs(parameterPhysicalValue(document(w.p).values,mid)-9)<1e-5&&std::abs(parameterPhysicalValue(document(w.p).values,high)-6)<1e-5&&std::abs(parameterPhysicalValue(document(w.p).values,tilt)+1)<1e-5);
    auto* cover=face->face.button("calibration");auto* close=face->revealed().button("close");auto* input=face->revealed().slider<studio::house::Knob>("input-trim");CHECK(cover&&close&&input);if(!cover||!close||!input)return;
    stackBandColorViewportMatrix(w,*stack,*unit,*face);
    w.editor->setSize(1080,680);pump(120);
    const auto before=document(w.p);const auto revision=w.revisions();cover->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    CHECK(face->isOpen()&&face->getHeight()==394&&stack->drawerRack().isNull()&&document(w.p)==before&&w.revisions()==revision);shot("band-color-editor-1080-open.png");
    input->grabKeyboardFocus();CHECK(face->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));pump();CHECK(!face->isOpen()&&cover->hasKeyboardFocus(false)&&document(w.p)==before&&w.revisions()==revision);
    cover->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));pump();input->grabKeyboardFocus();close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();CHECK(!face->isOpen()&&cover->hasKeyboardFocus(false));
    stack->openDrawer(id);pump();CHECK(stack->controls().parameters().size()==36);for(const auto index:stack->controls().parameters())CHECK(stack->controls().showParameter(index)&&stack->controls().knob(index));stack->closeDrawer();pump();
    const auto mode=*rackParameterIndex(NativeModule::saturation,6,"saturation.mode");auto full=document(w.p);full.values[mode]=0;CHECK(edit(w.p,full));pump();
    auto* drive=face->face.slider<studio::house::Knob>("mid-drive");CHECK(drive&&drive->isEnabled()&&drive->getDescription().containsIgnoreCase("retained"));
    if(drive){const auto retained=document(w.p);drive->keyPressed(juce::KeyPress(juce::KeyPress::upKey));pump();CHECK(document(w.p).values[mid]!=retained.values[mid]&&document(w.p).values[high]==retained.values[high]);CHECK(w.p.undoSoundEdit());pump();CHECK(document(w.p)==retained);}
    auto locked=document(w.p);locked.effectLocks[6]=true;locked.controlLocks[mid]=true;CHECK(edit(w.p,locked));CHECK(document(w.p)==locked);
    CHECK(wait("Band Color AI lock refresh",[&]{return unit->kept();}));CHECK(unit->kept());
    auto proposal=document(w.p);proposal.values[mid]=1;stack->setProposal(&proposal);CHECK(face->marks().empty());stack->setProposal(nullptr);
    CHECK(edit(w.p,before));pump();face->setOpen(true);pump();unit->fold.onToggle();pump();CHECK(unit->isFolded()&&!face->isOpen()&&!unit->on());unit->fold.onToggle();pump();
    w.editor->setSize(1547,965);pump();shot("band-color-editor-large-closed.png");face->setOpen(true);pump();shot("band-color-editor-large-open.png");
    const auto fallback=document(w.p);RackOperation change;change.kind=RackOperationKind::face;change.selected=id;for(const auto& rack:fallback.personalities->instances)if(rack.id==id)change.design=rack.design;
    CHECK(change.design.has_value());if(change.design){change.design->panel.controls[0].label="Changed";SoundDocument current;std::uint64_t at=0;CHECK(w.p.trySoundSnapshot(current,at)&&w.p.applyRackOperation(change,at)==RackCandidateStatus::ready);
      CHECK(wait("Band Color mismatch falls back",[&]{return stack->unit(id)&&!stack->unit(id)->face();}));CHECK(!stack->unit(id)->on()&&stack->unit(id)->getDescription().contains(" (bypassed)"));}
}
void stackCharacterDrive()
{
    std::cout<<"SECTION stack_character_drive\n";
    WindowRig w;auto* stack=w.stack();CHECK(stack);if(!stack)return;
    const auto factories=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Character Drive";});
    CHECK(found!=factories.end());if(found==factories.end())return;
    const auto id=placeFactory(w.p,std::size_t(found-factories.begin()),{RackRole::fx,masterChain,0},{{0,6}});
    CHECK(wait("Character Drive in the actual editor",[&]{return stack->unit(id)&&stack->unit(id)->face();}));
    auto* unit=stack->unit(id);if(!unit||!unit->face())return;auto* face=unit->face();
    auto& editor=*w.editor;editor.setSize(1547,965);pump(120);
    const auto shot=[&](const char* name){pump(80);if(output!=juce::File{})CHECK(writePng(output.getChildFile(name),snap(editor)));};
    const auto rackShot=[&](const char* name){if(output!=juce::File{})CHECK(writePng(output.getChildFile(name),snap(*face)));};
    CHECK(face->skin.maker=="OXIDE INSTRUMENTS"&&face->getHeight()==394&&!unit->on());
    CHECK(!face->getDescription().contains("DSP PENDING")&&unit->getDescription().contains(" (bypassed)"));
    shot("character-drive-editor-normal.png");rackShot("character-drive-native-default.png");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("character-drive-stack-hero.png"),snap(*unit)));
    auto* setup=face->face.button("setup");CHECK(setup);if(!setup)return;
    const auto before=document(w.p);const auto revisions=w.revisions();setup->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();
    CHECK(face->isOpen()&&face->getHeight()==394&&stack->drawerRack().isNull()&&document(w.p)==before&&w.revisions()==revisions);
    shot("character-drive-editor-setup.png");rackShot("character-drive-native-setup.png");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("character-drive-stack-hero-setup.png"),snap(*unit)));
    auto* mode=face->revealed().lever("mode");auto* all=face->revealed().button("all-controls");auto* close=face->revealed().button("close");CHECK(mode&&all&&close);if(!mode||!all||!close)return;
    mode->grabKeyboardFocus();CHECK(mode->hasKeyboardFocus(false));shot("character-drive-editor-focus.png");
    if(auto* input=face->face.slider<studio::house::Knob>("input")){auto* prior=face->face.slider<studio::house::Knob>("pre-tilt");CHECK(prior);if(prior){prior->grabKeyboardFocus();prior->moveKeyboardFocusToSibling(false);}pump();CHECK(input->hasKeyboardFocus(false));const auto readout=face->face.readoutBounds();CHECK(!readout.isEmpty()&&readout.getBottom()<=50);shot("character-drive-editor-touch-readout.png");mode->grabKeyboardFocus();pump();}else CHECK(false);
    mode->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump(120);
    CHECK(face->getDescription().contains("RETAINED"));shot("character-drive-editor-three-band.png");
    const auto modeIndex=*rackParameterIndex(NativeModule::saturation,6,"saturation.mode");
    CHECK(parameterPhysicalValue(document(w.p).values,modeIndex)==1);
    for(const auto* local:{"pre-tilt","character","drive","lane-mix","wet-trim"}){auto* c=face->face.control(local);CHECK(c&&c->getDescription().containsIgnoreCase("retained")&&c->isEnabled());}
    all->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump(120);
    CHECK(stack->drawerRack()==id&&stack->controls().parameters().size()==36&&stack->controls().keys()==std::vector<juce::String>({"GLOBAL","FULL","LOW","MID","HIGH"}));
    for(const auto key:stack->controls().parameters()){CHECK(stack->controls().showParameter(key));CHECK(stack->controls().knob(key)&&stack->controls().knob(key)->isVisible());}
    const auto printedInk=[](const juce::Image& image,juce::Rectangle<int> area){
      juce::Rectangle<int> ink;for(int y=area.getY();y<area.getBottom();++y)for(int x=area.getX();x<area.getRight();++x){const auto c=image.getPixelAt(x,y);if(std::min({c.getRed(),c.getGreen(),c.getBlue()})>178&&std::max({c.getRed(),c.getGreen(),c.getBlue()})-std::min({c.getRed(),c.getGreen(),c.getBlue()})<22)ink=ink.getUnion({x,y,1,1});}return ink;
    };
    const auto compactPages=[&](bool minimum){
      for(int page=0;page<5;++page){stack->controls().showKey(page);pump();CHECK(stack->controls().shownHeight()<=(page==0?634:320));
        const auto shown=stack->controls().shownKnobs();CHECK(shown.size()==(page==0?12:6));
        if(!shown.empty()){const auto bounds=stack->body().getLocalArea(&stack->controls(),stack->controls().getLocalBounds());stack->viewport().setViewPosition(0,std::max(0,bounds.getY()-8));}
        float left=1.e6f,right=0;for(const auto* k:shown){left=std::min(left,k->centre().x);right=std::max(right,k->centre().x);CHECK(k->entry.getY()>k->labelInk().getBottom());CHECK(k->entry.getBottom()<=stack->controls().shownHeight()-24);CHECK(stack->viewport().getLocalBounds().contains(stack->viewport().getLocalArea(&k->entry,k->entry.getLocalBounds())));CHECK(k->getPresentation()==(parameterRegistry()[k->index].kind==ParameterKind::continuous?studio::ProgrammerKnob::Presentation::rotary:parameterRegistry()[k->index].kind==ParameterKind::toggle?studio::ProgrammerKnob::Presentation::toggle:studio::ProgrammerKnob::Presentation::choice));}
        CHECK(left<250&&right>stack->controls().getWidth()-250);
        const auto panelImage=stack->controls().createComponentSnapshot(stack->controls().getLocalBounds(),true,1.f);
        for(int row=0;row<(page==0?2:1);++row){const auto top=62+190*row,bottom=top+174;float labelTop=1.e6f;
          const auto titleInk=printedInk(panelImage,{32,top+8,stack->controls().getWidth()-64,24});CHECK(!titleInk.isEmpty());
          for(const auto* k:shown)if((k->centre().y>300?1:0)==row){labelTop=std::min(labelTop,k->labelInk().getY());const auto valueInk=printedInk(panelImage,k->entry.getBounds());CHECK(!valueInk.isEmpty()&&valueInk.getBottom()<=bottom-8);if(juce::String(parameterRegistry()[k->index].id).contains(".saturation.")&&juce::String(parameterRegistry()[k->index].id).endsWith(".bypassed"))CHECK(k->label.getText()=="LANE BYPASS");}
          CHECK(titleInk.getBottom()<=labelTop-8);
        }
        shot(("character-drive-controls-"+juce::String(page)+(minimum?"-minimum.png":".png")).toRawUTF8());}
    };
    const auto patchBay=[&](bool minimum){
      CHECK(w.drawer()&&w.drawer()->detailsCollapsed()&&w.drawer()->drawerHeight()==0);
      auto* oldDetails=find(editor,"rack.drawer.details");CHECK(oldDetails&&!oldDetails->isShowing());
      auto* cover=dynamic_cast<juce::Button*>(find(editor,"rack.controls.patchbay"));CHECK(cover&&cover->isShowing());if(!cover)return;
      CHECK((bool)cover->getProperties()["sourceLoaded"]);
      CHECK(cover->getTitle().contains("Patch Bay")&&cover->getAccessibilityHandler()&&cover->getWidth()==248&&cover->getHeight()==54);
      auto* highKey=find(stack->controls(),"rack.controls.key.4");CHECK(highKey);if(highKey){highKey->grabKeyboardFocus();highKey->moveKeyboardFocusToSibling(true);pump();CHECK(cover->hasKeyboardFocus(false));auto* ring=dynamic_cast<studio::Ringed*>(cover);CHECK(ring&&ring->ringed());}
      shot(minimum?"character-drive-cover-focus-minimum.png":"character-drive-cover-focus.png");
      const auto unchanged=document(w.p);const auto rev=w.revisions();const auto page=stack->controls().shownKey();
      cover->grabKeyboardFocus();CHECK(static_cast<juce::Component*>(cover)->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));pump();
      CHECK(!w.drawer()->detailsCollapsed()&&w.drawer()->drawerHeight()>=400&&cover->getToggleState());
      auto* operation=find(*w.drawer(),"rack.operation");CHECK(operation&&operation->isShowing()&&operation->hasKeyboardFocus(true));
      if(output!=juce::File{})CHECK(writePng(output.getChildFile(minimum?"character-drive-cover-open-minimum.png":"character-drive-cover-open.png"),snap(*cover)));
      CHECK(cover->getDescription().containsIgnoreCase("open"));shot(minimum?"character-drive-patch-bay-open-minimum.png":"character-drive-patch-bay-open.png");
      CHECK(w.drawer()->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));pump();
      CHECK(w.drawer()->detailsCollapsed()&&w.drawer()->drawerHeight()==0&&!cover->getToggleState()&&cover->hasKeyboardFocus(false));
      CHECK(document(w.p)==unchanged&&w.revisions()==rev&&stack->controls().shownKey()==page);
      static_cast<juce::Component*>(cover)->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));pump();CHECK(cover->getToggleState());
      cover->grabKeyboardFocus();static_cast<juce::Component*>(cover)->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();CHECK(!cover->getToggleState()&&document(w.p)==unchanged&&w.revisions()==rev);
      if(output!=juce::File{})CHECK(writePng(output.getChildFile(minimum?"character-drive-cover-closed-minimum.png":"character-drive-cover-closed.png"),snap(*cover)));
      shot(minimum?"character-drive-patch-bay-closed-minimum.png":"character-drive-patch-bay-closed.png");
      if(!minimum){static_cast<juce::Component*>(cover)->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));pump();bool moved=false;for(const auto* other:stack->units())if(other->id()!=id){stack->select(other->id());pump();CHECK(stack->selected()==other->id());moved=true;break;}CHECK(moved);stack->select(id);pump();CHECK(stack->selected()==id);stack->openDrawer(id);pump();CHECK(stack->drawerRack()==id&&w.drawer()->detailsCollapsed()&&w.drawer()->drawerHeight()==0&&document(w.p)==unchanged&&w.revisions()==rev);}
    };
    // The comparison captures use real retained parameter state matching the approved drawing.
    auto drawing=document(w.p);
    for(const auto& [key,value]:std::initializer_list<std::pair<const char*,float>>{{"saturation.mode",0.f},{"saturation.full.character",1.f},{"saturation.full.drive_db",12.f},{"saturation.full.mix",.75f},{"saturation.mix",1.f},{"saturation.high.character",0.f},{"saturation.high.drive_db",0.f},{"saturation.high.mix",1.f}}){
        const auto i=rackParameterIndex(NativeModule::saturation,6,key);CHECK(i.has_value());if(i)drawing.values[*i]=w.p.control(*i).convertTo0to1(value);
    }
    CHECK(edit(w.p,drawing));pump(120);CHECK(!face->getDescription().contains("RETAINED"));
    patchBay(false);
    compactPages(false);
    const auto combined=[&](bool minimum){stack->controls().showKey(4);pump();stack->viewport().setViewPosition(0,std::max(0,unit->getY()-8));shot(minimum?"character-drive-combined-high-minimum.png":"character-drive-combined-high.png");};
    combined(false);
    const auto bypass=*rackParameterIndex(NativeModule::saturation,6,"bypassed");auto* requested=stack->controls().knob(bypass);CHECK(requested&&requested->getTitle()==parameterRegistry()[bypass].name);
    stack->closeDrawer();close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();CHECK(!face->isOpen());
    editor.setSize(InstrumentEditor::minimumWidth,InstrumentEditor::minimumHeight);pump(120);CHECK(editor.getWidth()==1080&&editor.getHeight()==680);shot("character-drive-editor-minimum-1080.png");
    setup->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));shot("character-drive-editor-minimum-setup.png");all->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));pump();compactPages(true);combined(true);patchBay(true);stack->closeDrawer();close->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    editor.setSize(1547,965);pump(120);
    const auto defaults=document(w.p);CHECK(defaults.values[modeIndex]==0);
    for(int end=0;end<2;++end){auto d=document(w.p);for(const auto& e:face->face.ctx.model.elements)if(e.view=="face"&&e.rackUnit>=0)d.values[e.paramIndex]=float(end);d.values[bypass]=0;CHECK(edit(w.p,d));pump(120);CHECK(unit->on()&&!face->getDescription().contains("DSP PENDING"));
      shot(end?"character-drive-editor-max.png":"character-drive-editor-min.png");rackShot(end?"character-drive-native-max.png":"character-drive-native-min.png");}
    CHECK(edit(w.p,defaults));pump(120);auto locked=document(w.p);const auto drive=*rackParameterIndex(NativeModule::saturation,6,"saturation.full.drive_db");locked.effectLocks[6]=true;locked.controlLocks[drive]=true;CHECK(edit(w.p,locked));pump(120);
    CHECK(unit->kept());shot("character-drive-editor-locks.png");locked.effectLocks[6]=false;CHECK(edit(w.p,locked));pump(120);
    auto candidate=document(w.p);candidate.values[drive]=1;candidate.values[*rackParameterIndex(NativeModule::saturation,6,"saturation.low.drive_db")]=.8f;candidate.values[*rackParameterIndex(NativeModule::saturation,6,"saturation.input_trim_db")]=.8f;stack->setProposal(&candidate);
    CHECK(!face->marks().empty()&&setup->getDescription().contains("SET-UP"));
    // Snapshot the real stack before the editor's timer replaces this direct test proposal with its current AI state.
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("character-drive-editor-proposals.png"),snap(editor)));stack->setProposal(nullptr);
    unit->fold.onToggle();pump();CHECK(unit->isFolded()&&!face->isOpen()&&!unit->on()&&unit->getDescription().contains(" (bypassed)"));shot("character-drive-editor-folded.png");
    unit->fold.onToggle();pump();
    auto unlocked=document(w.p);unlocked.controlLocks[drive]=false;CHECK(edit(w.p,unlocked));pump(120);
    const auto fallback=document(w.p);RackOperation change;change.kind=RackOperationKind::face;change.selected=id;for(const auto& i:fallback.personalities->instances)if(i.id==id)change.design=i.design;
    CHECK(change.design.has_value());if(!change.design)return;change.design->panel.controls[0].label="Input trim";SoundDocument live;std::uint64_t revision=0;CHECK(w.p.trySoundSnapshot(live,revision)&&w.p.applyRackOperation(change,revision)==RackCandidateStatus::ready);
    CHECK(wait("saturation fallback house face",[&]{return stack->unit(id)&&!stack->unit(id)->face();}));CHECK(!stack->unit(id)->on()&&stack->unit(id)->getDescription().contains(" (bypassed)"));shot("character-drive-editor-fallback.png");
    editor.setSize(InstrumentEditor::minimumWidth,InstrumentEditor::minimumHeight);pump(120);shot("character-drive-editor-minimum-fallback.png");
    // In a mixed EQ/Saturation rack, EQ retains its full header and the real rail OPERATIONS keeps service access.
    auto mixed=*found;mixed.id=juce::Uuid();mixed.name="Saturation and EQ service";mixed.units.push_back({1,NativeModule::dynamicEq,{}});
    RackOperation install;install.design=mixed;install.newInstance=juce::Uuid();install.destination={RackRole::fx,masterChain,1};install.allocation={{0,7},{1,0}};
    SoundDocument current;std::uint64_t at=0;CHECK(w.p.trySoundSnapshot(current,at)&&w.p.applyRackOperation(install,at)==RackCandidateStatus::ready);
    CHECK(wait("mixed EQ/Saturation service rack",[&]{return stack->unit(install.newInstance)!=nullptr;}));
    stack->openDrawer(install.newInstance);pump();auto* mixedUnit=stack->unit(install.newInstance);CHECK(mixedUnit);
    auto* mixedCover=dynamic_cast<juce::Button*>(find(stack->controls(),"rack.controls.patchbay"));CHECK(mixedCover&&mixedCover->isShowing());
    const auto mixedSound=document(w.p);const auto mixedRevision=w.revisions();
    if(mixedCover&&mixedUnit){static_cast<juce::Component*>(mixedCover)->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));pump();CHECK(!w.drawer()->detailsCollapsed());
      CHECK(stack->controls().showParameter(dynamicEqParameterStart+3));pump();CHECK(!mixedCover->isShowing());
      auto* selector=find(stack->controls(),"rack.controls.unit");CHECK(selector&&selector->isShowing());
      CHECK(w.drawer()->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));pump();CHECK(w.drawer()->detailsCollapsed()&&mixedUnit->operations.hasKeyboardFocus(false));
      mixedUnit->operations.triggerClick();pump();CHECK(!w.drawer()->detailsCollapsed()&&find(*w.drawer(),"rack.operation")->hasKeyboardFocus(false));
      shot("character-drive-mixed-eq-service.png");CHECK(w.drawer()->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));pump();
      CHECK(w.drawer()->detailsCollapsed()&&mixedUnit->operations.hasKeyboardFocus(false)&&document(w.p)==mixedSound&&w.revisions()==mixedRevision);
    }

}
void stackSaturationCrossover()
{
    std::cout<<"SECTION stack_saturation_crossover\n";
    const auto factories=airack::test::frozenFactoryRackDesigns();
    for(const auto* name:{"Character Drive","Band Color"}){
        const auto found=std::find_if(factories.begin(),factories.end(),[&](const auto& d){return d.name==name;});
        CHECK(found!=factories.end());if(found==factories.end())return;
        WindowRig w;auto* stack=w.stack();CHECK(stack);if(!stack)return;
        const auto id=placeFactory(w.p,std::size_t(found-factories.begin()),{RackRole::fx,masterChain,0},{{0,6}});
        CHECK(wait("saturation rack for native crossover",[&]{return stack->unit(id)!=nullptr;}));
        stack->openDrawer(id);pump(120);auto& panel=stack->controls();auto& editor=*w.editor;
        const auto prefix=juce::String(found->name=="Character Drive"?"character-drive":"band-color");
        const auto low=*rackParameterIndex(NativeModule::saturation,6,"saturation.low_crossover_hz");const auto high=low+1;
        auto* graph=dynamic_cast<studio::SaturationGraph*>(find(panel,"saturation.graph"));CHECK(graph);if(!graph)return;
        for(const bool minimum:{false,true}){
            editor.setSize(minimum?1080:1547,minimum?680:965);pump(120);
            for(int mode=0;mode<2;++mode){auto d=document(w.p);if(d.values[low-5]!=float(mode)){d.values[low-5]=float(mode);CHECK(edit(w.p,d));}pump(120);
                for(const int page:{0,1,4}){panel.showKey(page);pump();
                    const auto bounds=stack->body().getLocalArea(&panel,panel.getLocalBounds());stack->viewport().setViewPosition(0,std::max(0,bounds.getY()-8));pump();
                    CHECK(graph->isShowing()==(page==0));CHECK(panel.shownHeight()==(page==0?634:254));CHECK(panel.parameters().size()==36);
                    if(page==0){CHECK(graph->parameters()==std::vector<std::size_t>({low,high}));CHECK(graph->getDescription().contains(mode==0?"inactive":"Three-band"));CHECK(graph->getBottom()<=panel.shownHeight());CHECK(stack->viewport().getLocalBounds().contains(stack->viewport().getLocalArea(graph,graph->getLocalBounds())));}
                    if(output!=juce::File{})CHECK(writePng(output.getChildFile(prefix+"-crossover-"+(minimum?"minimum-":"normal-")+(mode==0?"full-mode-":"three-mode-")+juce::String(page)+".png"),snap(editor)));
                }
            }
        }
        editor.setSize(1547,965);panel.showKey(0);pump();
        auto clamp=document(w.p);clamp.values[low-5]=1;clamp.values[low]=w.p.control(low).convertTo0to1(2000);clamp.values[high]=w.p.control(high).convertTo0to1(500);clamp.values[low+4]=1;CHECK(edit(w.p,clamp));w.p.prepareToPlay(22050,64);pump(120);
        CHECK(graph->getDescription().contains("400")&&graph->getDescription().contains("9922.5"));
        const auto shot=[&](const char* suffix){const auto bounds=stack->body().getLocalArea(&panel,panel.getLocalBounds());stack->viewport().setViewPosition(0,std::max(0,bounds.getY()-8));pump();if(output!=juce::File{})CHECK(writePng(output.getChildFile(prefix+suffix),snap(editor)));};
        shot("-crossover-clamp.png");
        auto locked=document(w.p);locked.controlLocks[low]=true;CHECK(edit(w.p,locked));pump(120);CHECK(!graph->beginCrossoverGesture());shot("-crossover-locked.png");
        auto proposal=std::make_shared<SoundDocument>(document(w.p));proposal->values[high]=.8f;panel.showProposal(proposal);
        CHECK(graph->getDescription().containsIgnoreCase("proposal"));if(output!=juce::File{})CHECK(writePng(output.getChildFile(prefix+"-crossover-proposal.png"),snap(editor)));panel.showProposal(nullptr);
        auto* cover=dynamic_cast<juce::Button*>(find(panel,"rack.controls.patchbay"));CHECK(cover&&cover->isShowing());if(cover){cover->triggerClick();pump(120);CHECK(!w.drawer()->detailsCollapsed());shot("-crossover-cover-open.png");CHECK(w.drawer()->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));pump();CHECK(cover->hasKeyboardFocus(false));}
        locked.controlLocks[low]=false;CHECK(edit(w.p,locked));pump(120);const auto beforeHide=document(w.p);CHECK(graph->beginCrossoverGesture());stack->setVisible(false);
        CHECK(w.p.beginParameterGesture(0));CHECK(w.p.endParameterGesture(0));CHECK(!graph->dragCrossovers(1000,5000));CHECK(document(w.p)==beforeHide);stack->setVisible(true);
        CHECK(graph->beginCrossoverGesture());stack->closeDrawer();CHECK(!graph->dragCrossovers(1000,5000));
    }
}
void postedClickBehindSlowMessage()
{
    std::cout<<"SECTION posted_click_behind_slow_message\n";
    CHECK(clickRunsBehindSlowMessage([](juce::Button& button) { click(button); }));
}
// Terminal: stopping dispatch makes JUCE destroy these actual retirement callables without invoking them.
// The real retired controls must disappear and end their held gestures before their first host frame is released.
void stackRetirementRejectedPosts()
{
    std::cout<<"SECTION stack_retirement_rejected_posts"<<std::endl;
    struct Probe
    {
        juce::Component::SafePointer<studio::MakerFace> retired;
        HouseRig::Counter gestures;
        int issued=0;
        bool watching=false,released=false;
        struct Frame
        {
            Probe& probe;bool retirement;
            Frame(Probe& p,bool r):probe(p),retirement(r){}
            ~Frame(){if(retirement){CHECK(probe.retired==nullptr&&probe.gestures.counts.ends==1);probe.released=true;}}
        };
    };
    const auto peers=juce::ComponentPeer::getNumPeers();
    std::array<Probe,2> probes;
    std::array<std::unique_ptr<StackRig>,2> rigs;
    std::array<SoundDocument,2> recalled;
    std::array<juce::MemoryBlock,2> bytes;
    const auto factories=airack::test::frozenFactoryRackDesigns();
    const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.name=="Band Color";});
    CHECK(found!=factories.end());if(found==factories.end())return;
    const auto index=*rackParameterIndex(NativeModule::saturation,6,"saturation.mid.drive_db");
    for(std::size_t n=0;n<rigs.size();++n)
    {
        studio::house::Host host;
        host.frame=[&probes,n]{auto& p=probes[n];return std::make_shared<Probe::Frame>(p,p.watching&&p.issued++==0);};
        rigs[n]=std::make_unique<StackRig>(blankInstance,std::move(host));auto& r=*rigs[n];
        const auto empty=document(r.p);
        const auto id=placeFactory(r.p,std::size_t(found-factories.begin()),{RackRole::fx,masterChain,0},{{0,6}});
        r.refresh();auto* unit=r.stack->unit(id);auto* face=unit?unit->face():nullptr;CHECK(face);if(!face)return;
        auto* knob=face->face.slider<studio::house::Knob>("mid-drive");CHECK(knob);if(!knob)return;
        probes[n].retired=face;const auto before=document(r.p);
        RackOperation change;change.kind=RackOperationKind::face;change.selected=id;
        for(const auto& rack:before.personalities->instances)if(rack.id==id)change.design=rack.design;
        CHECK(change.design.has_value());if(!change.design)return;change.design->panel.controls[0].label="Changed";
        RackDifference difference;CHECK(makeRackCandidate(before,change,SoundEditOrigin::manual,recalled[n],difference)==RackCandidateStatus::ready);
        if(n==1)recalled[n]=empty;
        CHECK(encodeSoundState(recalled[n],bytes[n]));
        r.p.control(index).addListener(&probes[n].gestures);knob->onDragStart();
        CHECK(probes[n].gestures.counts.begins==1&&probes[n].gestures.counts.ends==0);
    }
    auto* manager=juce::MessageManager::getInstance();manager->stopDispatchLoop();
    bool invoked=false;CHECK(manager->hasStopMessageBeenSent());
    CHECK(!juce::MessageManager::callAsync([&invoked]{invoked=true;}));CHECK(!invoked);
    for(std::size_t n=0;n<rigs.size();++n)
    {
        auto& p=probes[n];auto& r=*rigs[n];p.watching=true;
        r.p.setStateInformation(bytes[n].getData(),(int)bytes[n].getSize());r.refresh();
        CHECK(p.retired==nullptr&&p.released&&p.issued>0);
        CHECK(p.gestures.counts.begins==1&&p.gestures.counts.ends==1&&document(r.p)==recalled[n]);
        p.watching=false;r.p.control(index).removeListener(&p.gestures);
        CHECK(r.p.beginParameterGesture(index)&&r.p.endParameterGesture(index));
        std::cout<<"  rejected "<<(n==0?"face":"unit")<<": deleted before retirement frame release, gestures "<<p.gestures.counts.begins<<'/'<<p.gestures.counts.ends<<std::endl;
        rigs[n].reset();
    }
    CHECK(!invoked&&juce::ComponentPeer::getNumPeers()==peers);
}
}
// --only stack,window: only the sections whose functions' names start with one of these, for a quick local run.
juce::StringArray only;
int partition=0, currentPartition=1;
bool listSections=false;
bool runs(const char* name)
{
    if(partition!=0 && partition!=currentPartition)return false;
    for(const auto& prefix:only)if(juce::String(name).startsWith(prefix))return true;
    return only.isEmpty();
}
const auto studioMeasurementStart=std::chrono::steady_clock::now();
auto studioMeasurementMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-studioMeasurementStart).count();}
#define RUN(section) do { if(runs(#section)) { std::cout<<"RUN_SECTION "<<#section<<std::endl;if(!listSections){const auto began=studioMeasurementMs();std::cout<<"MEASURE studio phase=begin section="<<#section<<" elapsed_ms="<<began<<" checks="<<checks<<" failures="<<failures<<std::endl;section();const auto ended=studioMeasurementMs();std::cout<<"MEASURE studio phase=end section="<<#section<<" elapsed_ms="<<ended<<" duration_ms="<<ended-began<<" checks="<<checks<<" failures="<<failures<<std::endl;}} } while(false)
struct LumenStackRig:StackRig
{
    LumenStackRig()
    {
        const auto factory=airack::test::frozenFactoryRackDesigns();const auto found=std::find_if(factory.begin(),factory.end(),[](const auto& d){return d.id==juce::Uuid("cc73c36a-696e-4b09-9cf3-dcd4dfac16f0");});
        CHECK(found!=factory.end());if(found==factory.end())return;
        int position=0;for(const std::uint8_t slot:{std::uint8_t(2),std::uint8_t(7)}){RackOperation op;op.design=*found;op.newInstance=juce::Uuid();op.destination={RackRole::fx,masterChain,std::uint8_t(position++)};op.allocation={{0,slot}};SoundDocument d;std::uint64_t revision=0;CHECK(p.trySoundSnapshot(d,revision)&&p.applyRackOperation(op,revision)==RackCandidateStatus::ready);}
        refresh();pump();
    }
};
void stackTileArrayDrawer()
{
    std::cout<<"SECTION stack_tile_array_drawer\n";
    StackRig r;
    const auto place=[&](const RackDesign& design){SoundDocument sound;std::uint64_t edit=0;CHECK(r.p.trySoundSnapshot(sound,edit));RackOperation op;op.design=design;op.newInstance=juce::Uuid();op.destination={design.role,masterChain,0};CHECK(r.p.applyRackOperation(op,edit)==RackCandidateStatus::ready);return op.newInstance;};
    const auto tile=place(airack::test::frozenTileArrayRackDesign());
    const auto following=place(airack::test::frozenExampleRackDesigns()[4]);
    r.refresh();auto* unit=r.stack->unit(tile);auto* next=r.stack->unit(following);CHECK(unit&&next);if(!unit||!next)return;
    auto* face=unit->face();CHECK(face);if(!face)return;CHECK(face->units()==4&&unit->units()==4);
    const auto closed=unit->getHeight(),followingTop=next->getY();face->setOpen(true,false);pump();
    CHECK(face->isOpen()&&face->units()==8&&face->shownUnits()==8&&unit->units()==8);
    CHECK(std::abs(unit->getHeight()-closed-4*r.stack->unitHeight())<=1);
    CHECK(std::abs((next->getY()-followingTop)-(unit->getHeight()-closed))<=1);
    CHECK(r.at(face->revealed()).getBottom()<=r.at(*unit).getBottom()+1);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-tile-array-open.png"),snap(*unit)));
    unit->setFolded(true);pump();CHECK(!face->isOpen()&&unit->units()==1);if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-tile-array-folded.png"),snap(*unit)));unit->setFolded(false);pump();CHECK(unit->units()==4&&!face->isOpen());
}
void stackLumen()
{
    std::cout<<"SECTION stack_lumen_factory_two_placements_native_viewport_and_all97_controls\n";LumenStackRig r;CHECK(r.ids().size()==2);
    const auto skin=juce::JSON::parse(airack::test::frozenMakerSkinFile("lumen-field","elements.json").toString());const auto rect=skin["reveal"]["native_editor"]["rect"];
    for(int n=0;n<2;++n){auto* unit=r.unit(n);auto* face=unit?unit->face():nullptr;auto* graph=unit?unit->spaceGraph():nullptr;CHECK(unit&&face&&graph);if(!unit||!face||!graph)return;
        const auto slot=std::uint8_t(n==0?2:7);CHECK(face->skin.design==juce::Uuid("cc73c36a-696e-4b09-9cf3-dcd4dfac16f0")&&unit->units()==4&&!face->isOpen()&&!graph->isShowing());
        CHECK(!unit->row.isShowing()&&!unit->up.isShowing()&&!unit->down.isShowing()&&unit->operations.isShowing());
        CHECK(graph->boundSlot()==std::optional<std::uint8_t>(slot)&&graph->getParentComponent()==&face->revealed());
        auto* size=face->face.slider<studio::house::Knob>("size");CHECK(size&&size->element.paramIndex==*rackParameterIndex(NativeModule::spaceReverb,slot,"space_reverb.size"));
        const auto before=document(r.p);const auto revisions=r.revisions();face->setOpen(true,false);CHECK(face->isOpen()&&unit->units()==7&&graph->isShowing()&&graph->isEnabled());
        CHECK(document(r.p)==before&&r.revisions()==revisions);
        CHECK(rect.isArray()&&rect.size()==4);if(rect.isArray()&&rect.size()==4){const juce::Rectangle<float> expected{float(double(rect[0])),float(double(rect[1])),float(double(rect[2])),float(double(rect[3]))};CHECK(graph->getBounds()==expected.toNearestInt());}
        CHECK(face->revealed().getLocalBounds().contains(graph->getBounds())&&graph->getProperties()["maker.theme"].toString()=="amber_crt");
        const auto glass=snap(*graph).getPixelAt(0,0);CHECK(glass.getRed()>glass.getBlue());
        for(const auto* id:{"space.decay","space.post.eq","space.band","space.add","space.active","space.bypass","space.delete","space.controls"})CHECK(find(*graph,id));
        for(auto* child:graph->getChildren())if(child->isVisible())CHECK(graph->getLocalBounds().contains(child->getBounds()));
        r.stack->openDrawer(unit->id());CHECK(r.stack->controls().parameters().size()==97);
        for(const auto parameter:unitReach(NativeModule::spaceReverb,slot)){CHECK(r.stack->controls().showParameter(parameter));CHECK(!r.stack->controls().detectorPage());CHECK(r.stack->controls().knob(parameter));}
        r.stack->closeDrawer();face->setOpen(false,false);CHECK(unit->units()==4&&!graph->isShowing());
    }
    // Editing a focused label drops only that placement's maker, retaining its live house graph and banks.
    auto changed=document(r.p);auto personalities=*changed.personalities;personalities.instances[0].design.panel.controls[0].label="Room";changed.personalities=std::make_shared<const RackPersonalityState>(std::move(personalities));CHECK(restore(r.p,state(changed)));r.refresh();pump();
    CHECK(r.unit(0)&&!r.unit(0)->face()&&r.unit(0)->spaceGraph()&&r.unit(0)->spaceGraph()->isShowing()&&r.unit(0)->units()==2&&r.unit(1)&&r.unit(1)->face());
}
void stackLumenProposal()
{
    std::cout<<"SECTION stack_lumen_hidden_graph_proposal_marks_voicing_opener\n";LumenStackRig r;auto* unit=r.unit(0);auto* face=unit?unit->face():nullptr;auto* graph=unit?unit->spaceGraph():nullptr;CHECK(face&&graph);if(!face||!graph)return;
    auto* pull=face->front()?face->front()->button("voicing"):nullptr;CHECK(pull);if(!pull)return;const auto live=document(r.p);auto candidate=live;
    const auto frequency=*rackParameterIndex(NativeModule::spaceReverb,2,"space_reverb.decay.1.freq_hz");candidate.values[frequency]=r.p.control(frequency).convertTo0to1(3200);
    r.stack->setProposal(&candidate);CHECK(graph->proposed()&&unit->proposed()&&!unit->hiddenProposal()&&!graph->isShowing());CHECK(pull->getDescription().contains("1")&&pull->getDescription().contains("voicing"));
    face->setOpen(true,false);CHECK(graph->isShowing()&&pull->getDescription().isEmpty());face->setOpen(false,false);CHECK(pull->getDescription().isNotEmpty());
    auto locked=live;locked.controlLocks[frequency]=true;CHECK(edit(r.p,locked));r.refresh();r.stack->setProposal(&candidate);CHECK(!graph->proposed()&&pull->getDescription().isEmpty());
    r.stack->setProposal(nullptr);CHECK(!unit->proposed());
}
void stackLumenLifetime()
{
    std::cout<<"SECTION stack_lumen_close_fold_focus_and_destruction_end_native_graph_gestures\n";
    for(int action=0;action<4;++action){LumenStackRig r;auto* unit=r.unit(0);auto* face=unit?unit->face():nullptr;auto* graph=unit?unit->spaceGraph():nullptr;CHECK(unit&&face&&graph);if(!unit||!face||!graph)return;
        face->setOpen(true,false);auto* pull=face->front()?face->front()->button("voicing"):nullptr;CHECK(pull);if(!pull)return;const auto frequency=*rackParameterIndex(NativeModule::spaceReverb,2,"space_reverb.decay.1.freq_hz");
        HouseRig::Counter listener;r.p.control(frequency).addListener(&listener);const auto before=document(r.p);const auto start=graph->nodePosition(0);
        graph->mouseDown(event(*graph,start,juce::ModifierKeys::leftButtonModifier,1,false));graph->mouseDrag(event(*graph,start+Pt(-24,-12),juce::ModifierKeys::leftButtonModifier,1,true,start));const auto dragged=document(r.p);CHECK(dragged!=before&&listener.counts.begins==1&&listener.counts.ends==0);
        if(action==0){face->setOpen(false,false);CHECK(pull->hasKeyboardFocus(false)&&!graph->isShowing());}
        if(action==1){unit->setFolded(true);CHECK(!face->isOpen()&&!graph->isShowing());unit->setFolded(false);CHECK(!face->isOpen()&&!graph->isShowing());}
        if(action==2)graph->focusLost(juce::Component::focusChangedDirectly);
        if(action==3){r.stack.reset();pump();}
        CHECK(listener.counts.begins==1&&listener.counts.ends==1&&document(r.p)==dragged);CHECK(r.p.undoSoundEdit()&&document(r.p)==before);r.p.control(frequency).removeListener(&listener);
    }
}
// Small Hours on the shared master after Thread Press's two effects, its reverb in effect bank 6; `twice` places a
// second one after it, its reverb in bank 7.
void placeSmallHours(InstrumentProcessor& p,bool twice=false)
{
    placeRack(p,4,{RackRole::fx,masterChain,2},{{0,5}});
    if(twice)placeRack(p,4,{RackRole::fx,masterChain,3},{{0,6}});
}
const juce::StringArray smallHoursClosed{"rack.fold","face.program","face.afterglow","face.depth","face.swell","face.age","face.wow","face.grit","face.mix","face.in",
                                         "face.adjust","rack.operations"};
// Small Hours' faceplate in the stack (the Small Hours rack spec): SMALL HOURS is a 3U unit showing NACHTFELD's
// faceplate between the rails, bound to its own effect bank, a second one to its own, instead of its identity block, UP,
// DOWN and row, its lamp lit while it is IN; Tab runs its triangle, the face's controls, the lid and OPERATIONS; a press
// on AFTERGLOW's flywheel, a PROGRAM station, an AGE detent, IN / OUT or the lid never drags, a drag on the flywheel being
// one gesture, and one on the left ear, the bare plate or the needle selects it and a 20 px drag lifts it. With
// --images: stack-small-hours.png.
void stackSmallHours()
{
    std::cout<<"SECTION stack_small_hours\n";
    StackRig r;
    drawnRacks(r.p);placeSmallHours(r.p,true);r.refresh();pump();
    auto* small=r.unit(3);auto* second=r.unit(4);auto* face=small!=nullptr?small->face():nullptr;
    CHECK(face!=nullptr&&second!=nullptr&&second->face()!=nullptr);
    if(face==nullptr||second==nullptr||second->face()==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();
    CHECK(find(*small,"rack.face")==face&&face->isShowing()&&!face->isOpen()&&small->units()==3&&std::abs(r.at(*small).getHeight()-3.0f*u)<=1.0f);
    CHECK(closeTo(r.at(*face),Rect((float)studio::HouseUnit::railW*k,r.at(*small).getY(),2140.0f*k,591.0f*k)));
    CHECK(!lettered(*small,"SMALL HOURS")&&!lettered(*small,"SHARED MASTER")&&!small->up.isShowing()&&!small->down.isShowing()&&!small->row.isShowing()
          &&small->operations.isShowing());
    CHECK(small->name()=="SMALL HOURS"&&small->getTitle()=="Small Hours");
    const auto d=document(r.p);
    CHECK(small->getDescription()==rackHeaderText(d,d.personalities->instances[3])+"\nNACHTFELD, SMALL HOURS");
    auto* afterglow=face->face.slider<studio::house::Knob>("afterglow");auto* program=face->face.lever("program");auto* age=face->face.lever("age");
    auto* in=face->face.lever("in");auto* lid=face->face.button("adjust");auto* lamp=face->face.lamp("lamp");auto* needle=face->face.needle("needle");
    CHECK(afterglow!=nullptr&&program!=nullptr&&age!=nullptr&&in!=nullptr&&lid!=nullptr&&lamp!=nullptr&&needle!=nullptr);
    if(afterglow==nullptr||program==nullptr||age==nullptr||in==nullptr||lid==nullptr||lamp==nullptr||needle==nullptr)return;
    CHECK(afterglow->element.paramIndex==idx("effect.6.era_reverb.decay_seconds")&&needle->element.paramIndex==idx("effect.6.era_reverb.decay_seconds")
          &&program->element.paramIndex==idx("effect.6.era_reverb.voicing")&&age->element.paramIndex==idx("effect.6.era_reverb.age")
          &&in->element.paramIndex==idx("effect.6.bypassed")&&lamp->element.paramIndex==idx("effect.6.bypassed")&&lamp->lit());
    if(auto* damping=face->revealed().slider<studio::house::Knob>("damping"))CHECK(damping->element.paramIndex==idx("effect.6.era_reverb.damping_hz"));else CHECK(!"no DAMPING");
    if(auto* a=second->face()->face.slider<studio::house::Knob>("afterglow"))CHECK(a->element.paramIndex==idx("effect.7.era_reverb.decay_seconds"));else CHECK(!"no second AFTERGLOW");
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-small-hours.png"),r.body()));
    juce::StringArray order;
    for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(small->isParentOf(c))order.add(c->getComponentID());
    CHECK(order==smallHoursClosed);
    if(order!=smallHoursClosed)std::cout<<"  Tab order: "<<order.joinIntoString(" ")<<'\n';
    // Thread Press selected and Small Hours in view; each press 20 px up.
    const auto ids=r.ids();
    Pointer m{*r.stack};
    const auto show=[&]{r.stack->select(ids[2]);r.stack->reveal(ids[3]);pump();};
    const auto at=[&](juce::Component& c,Pt p){return r.stack->getLocalPoint(&c,p);};
    const auto held=[&](Pt from){m.press(from);m.move(from+Pt(0.0f,-10.0f));m.move(from+Pt(0.0f,-20.0f));return r.stack->dragging();};
    {
        show();
        const auto index=afterglow->element.paramIndex;const auto was=r.p.control(index).getValue();
        HouseRig::Counter gestures;r.p.control(index).addListener(&gestures);
        const auto from=at(*afterglow,local(*afterglow,afterglow->element.pivot-Pt(0.0f,60.0f)));
        CHECK(!held(from));
        m.release(from+Pt(0.0f,-20.0f));
        r.p.control(index).removeListener(&gestures);
        CHECK(!r.stack->dragging()&&r.p.control(index).getValue()>was&&r.stack->selected()==ids[3]&&gestures.counts.begins==1&&gestures.counts.ends==1);
    }
    for(const auto& [lever,position,legend]:{std::tuple{program,std::size_t(4),"ROOM"},std::tuple{age,std::size_t(0),"CLEAR"}})
    {
        show();
        const auto from=at(*lever,local(*lever,lever->element.positions[position].legendRect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&lever->legend()==legend);
    }
    {
        show();
        const auto b=in->element.paramIndex;const auto was=r.p.control(b).getValue();
        const auto from=at(*in,local(*in,in->element.rect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&r.p.control(b).getValue()!=was&&!lamp->lit()&&in->legend()=="OUT");
    }
    {
        show();
        const auto from=at(*lid,local(*lid,lid->element.rect.getCentre()));
        CHECK(!held(from));
        m.release(from);
        CHECK(!r.stack->dragging()&&face->isOpen());
        face->setOpen(false);pump();
    }
    // The left ear, the bare plate under SWELL and AGE, and the needle at the recipe's 8 s.
    for(const auto spot:{Pt(40.0f,300.0f),Pt(800.0f,395.0f),Pt(1204.56f,89.54f)})
    {
        show();juce::Component::unfocusAllComponents();pump();
        const auto from=at(face->face,spot);
        m.press(from);
        CHECK(r.stack->selected()==ids[3]&&!face->hasKeyboardFocus(true));
        m.move(from+Pt(0.0f,-20.0f));
        CHECK(r.stack->dragging());
        m.escape();m.release(m.last);
        CHECK(!r.stack->dragging());
    }
}
// The lid in the stack (the Small Hours rack spec): it opens in place, writing nothing, the unit keeping its 3U and the
// space under it where it was; Tab reaches the seven trims and the open lid's strip only while it is open; closing with
// focus inside hands it to the lid; folding to the host's strip closes it, the strip's LED lit while the reverb is IN, and
// unfolded it shows closed. With --images: stack-small-hours-open.png and stack-small-hours-folded.png.
void stackSmallHoursLid()
{
    std::cout<<"SECTION stack_small_hours_lid\n";
    StackRig r;
    drawnRacks(r.p);placeSmallHours(r.p);r.refresh();pump();
    auto* small=r.unit(3);auto* face=small!=nullptr?small->face():nullptr;
    auto* lid=face!=nullptr?face->face.button("adjust"):nullptr;auto* close=face!=nullptr?face->revealed().button("close"):nullptr;
    CHECK(lid!=nullptr&&close!=nullptr);
    if(lid==nullptr||close==nullptr)return;
    const auto k=r.stack->scale(),u=r.stack->unitHeight();const auto before=r.revisions();const auto spaceTop=r.at(r.space).getY();
    const auto order=[&]{juce::StringArray out;for(auto* c:juce::KeyboardFocusTraverser().getAllComponents(&r.holder))if(small->isParentOf(c))out.add(c->getComponentID());return out;};
    auto open=smallHoursClosed;open.removeString("rack.operations");
    open.addArray(juce::StringArray({"face.pre-delay","face.diffusion","face.bass","face.damping","face.speed","face.low-cut","face.high-cut","face.close","rack.operations"}));
    CHECK(order()==smallHoursClosed);
    const auto space=juce::KeyPress(juce::KeyPress::spaceKey);
    lid->keyPressed(space);pump();
    CHECK(face->isOpen()&&face->revealed().isShowing()&&small->units()==3&&std::abs(r.at(*small).getHeight()-3.0f*u)<=1.0f&&std::abs(r.at(r.space).getY()-spaceTop)<=1.0f
          &&r.revisions()==before);
    CHECK(order()==open);
    if(order()!=open)std::cout<<"  open Tab order: "<<order().joinIntoString(" ")<<'\n';
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-small-hours-open.png"),r.body()));
    if(auto* damping=face->revealed().slider<studio::house::Knob>("damping")){damping->grabKeyboardFocus();pump();}else CHECK(!"no DAMPING");
    close->keyPressed(space);pump();
    CHECK(!face->isOpen()&&!face->revealed().isShowing()&&lid->hasKeyboardFocus(false)&&order()==smallHoursClosed&&r.revisions()==before);
    lid->keyPressed(space);pump();
    pressRail(small->fold);pump();
    CHECK(small->isFolded()&&small->units()==1&&std::abs(r.at(*small).getHeight()-u)<=1.0f&&!face->isShowing()&&!face->isOpen()&&!small->operations.isShowing()
          &&r.revisions()==before);
    const auto tapeW=std::max(180.0f,studio::house::TapeArt::widthFor(r.model.house,"SMALL HOURS",54.0f));
    CHECK(meanLuma(snap(*small),Rect(8.0f,8.0f).withCentre(Pt((194.0f+tapeW+40.0f)*k,(float)small->getHeight()*0.5f)))>150.0);
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-small-hours-folded.png"),r.body()));
    pressRail(small->fold);pump();
    CHECK(!small->isFolded()&&small->face()==face&&face->isShowing()&&!face->isOpen()&&small->units()==3&&small->operations.isShowing()&&!small->row.isShowing()
          &&order()==smallHoursClosed&&r.revisions()==before);
}
// The AI's marks on Small Hours: the stack hands the proposal to its faceplate, not its hidden row, the lid saying what
// it covers; the same candidate again draws nothing; folded, the tape has the outline; none clears them. With
// --images: stack-small-hours-marks.png.
void stackSmallHoursMarks()
{
    std::cout<<"SECTION stack_small_hours_marks\n";
    StackRig r;
    drawnRacks(r.p);placeSmallHours(r.p);r.refresh();pump();
    auto* small=r.unit(3);auto* face=small!=nullptr?small->face():nullptr;
    CHECK(face!=nullptr);
    if(face==nullptr)return;
    auto* afterglow=face->face.slider<studio::house::Knob>("afterglow");auto* in=face->face.lever("in");auto* lid=face->face.button("adjust");
    CHECK(afterglow!=nullptr&&in!=nullptr&&lid!=nullptr);
    if(afterglow==nullptr||in==nullptr||lid==nullptr)return;
    const auto t=afterglow->element.paramIndex,b=in->element.paramIndex,dm=idx("effect.6.era_reverb.damping_hz");
    auto candidate=document(r.p);candidate.values[t]=r.p.control(t).convertTo0to1(16.0f);candidate.values[b]=1.0f;candidate.values[dm]=r.p.control(dm).convertTo0to1(3000.0f);
    r.stack->setProposal(&candidate);pump();
    CHECK(face->marks().size()==3&&!face->proposesPanel()&&small->proposed()&&small->row.marks().empty()&&!small->row.proposesPanel());
    CHECK(afterglow->getDescription()=="AI proposes 16.00 s"&&in->getDescription()=="AI proposes OUT"&&lid->getDescription()=="AI proposes changes to 1 adjust lid control"
          &&!small->getDescription().contains("AI proposes"));
    if(output!=juce::File{})CHECK(writePng(output.getChildFile("stack-small-hours-marks.png"),r.body()));
    const auto copy=candidate;
    CHECK(paintsOver(*face,[&]{for(int n=0;n<20;++n){r.stack->setProposal(&copy);r.refresh();pump(25);}})==0);
    small->fold.onToggle();pump();
    CHECK(small->isFolded()&&tapeOutline(*r.stack,*small)>=40);
    small->fold.onToggle();pump();
    r.stack->setProposal(nullptr);pump();
    CHECK(face->marks().empty()&&!small->proposed()&&!small->getDescription().contains("AI proposes")&&afterglow->getDescription().isEmpty()&&lid->getDescription().isEmpty());
}
// The LIBRARY's preview of Small Hours: its closed faceplate composed at the design's values, 3U, dimmed to half, under
// the dashes, with its letters on a dark band. With --images: stack-small-hours-preview.png.
void stackSmallHoursPreview()
{
    std::cout<<"SECTION stack_small_hours_preview\n";
    StackRig r;
    drawnRacks(r.p);r.refresh();pump();
    const auto small=airack::test::frozenExampleRackDesigns()[4];const auto skin=studio::makerFaceFor(small);
    CHECK(skin!=nullptr);
    if(skin==nullptr)return;
    const auto u=r.stack->unitHeight();const auto spaceTop=r.at(r.space).getY();
    const StackPlacement preview{{RackRole::fx,masterChain,2},"PREVIEW: FX 2, AFTER THREAD PRESS",rackPreviewInstance(small),rackPreviewValues(small)};
    r.stack->setPlacement(preview);pump();
    auto* s=r.stack->placement();
    CHECK(s!=nullptr);
    if(s==nullptr)return;
    const auto k=r.stack->scale();const auto place=r.at(*s);
    CHECK(s->getTitle()==preview.letters&&s->getDescription()=="Small Hours"&&std::abs(place.getHeight()-3.0f*u)<=1.0f&&std::abs(r.at(r.space).getY()-(spaceTop+3.0f*u))<=1.0f);
    auto* first=find(*s,"rack.focus.0");
    CHECK(!lettered(*s,"SMALL HOURS")&&!lettered(*s,preview.letters)&&(first==nullptr||!first->isShowing()));
    const auto composed=studio::MakerFace::composeFor(*skin,*preview.preview,rackPreviewParameters(small));
    CHECK(composed.getWidth()==2140&&composed.getHeight()==591);
    if(!composed.isValid())return;
    // Away from the dashes and the band: the left ear, the dial's window by its 1 s step, and the lid.
    const auto img=r.body();
    for(const auto area:{Rect(40.0f,40.0f,120.0f,60.0f),Rect(700.0f,50.0f,150.0f,60.0f),Rect(1300.0f,480.0f,200.0f,50.0f)})
    {
        const auto shown=meanLuma(img,(area*k).translated((float)studio::HouseUnit::railW*k,place.getY())),drawn=meanLuma(composed,area);
        CHECK(std::abs(shown-(drawn*0.5+2.0))<8.0);
        std::cout<<"  "<<area.toString()<<": shown "<<shown<<", composed "<<drawn<<'\n';
    }
    const auto middle=Rect(773.0f-150.0f,place.getCentreY()-10.0f,300.0f,20.0f).getSmallestIntegerContainer();
    double between=0;int n=0;
    for(int y=middle.getY();y<middle.getBottom();++y)for(int x=middle.getX();x<middle.getRight();++x)
        if(const auto l=luma(img.getPixelAt(x,y));l<60.0){between+=l;++n;}
    CHECK(peakLuma(img,middle.toFloat())>150.0&&n>0&&between/n<25.0);
    if(output!=juce::File{}){r.stack->viewport().setViewPosition(0,(int)place.getY()-200);pump();CHECK(writePng(output.getChildFile("stack-small-hours-preview.png"),r.body()));}
}
void stackLibraryPreparation(){
    std::cout<<"SECTION stack_library_preparation_actual_caller\n";
    struct Record {std::vector<airack::RackInstance> used,requested;std::optional<airack::RackInstance> preview;};
    struct Records {std::mutex mutex;std::vector<Record> values;};const auto records=std::make_shared<Records>();
    airack::setRackLibrarySkinTestHook([records](auto used,auto preview,auto requested,bool completed,bool){if(completed)return;const std::lock_guard lock(records->mutex);records->values.push_back({std::move(used),std::move(requested),std::move(preview)});});
    const auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task5/fix1/ui-"+juce::Uuid().toString());auto runtime=std::make_shared<airack::RackLibraryRuntime>();runtime->store=std::make_shared<airack::RackPackageStore>(airack::RackLibraryPaths{root});std::atomic<int> requests{0};runtime->client=std::make_shared<airack::RackLibraryClient>(runtime->store,[&](const auto&,const auto&,const auto&,auto&){++requests;return airack::RackLibraryStatus::networkFailure;});
    {
        airack::InstrumentProcessor processor(runtime);CHECK(blankInstance(processor));const auto factories=airack::test::frozenFactoryRackDesigns();const auto source=factories.front();const auto effect=std::find_if(factories.begin(),factories.end(),[](const auto& design){return design.role==airack::RackRole::fx&&design.units.size()==1;});CHECK(effect!=factories.end());if(effect==factories.end())return;
        for(unsigned i=0;i<12;++i){const bool synth=i<4;airack::RackOperation op;op.design=synth?source:*effect;op.design->id=juce::Uuid();op.design->name="used-"+juce::String(i);op.newInstance=juce::Uuid();op.allowSlotReuse=synth&&i==0;op.destination={synth?airack::RackRole::synth:airack::RackRole::fx,synth||i<8?airack::masterChain:std::uint8_t(0),std::uint8_t(synth?i:i<8?i-4:i-8)};op.allocation={{0,std::uint8_t(synth?i:i-4)}};auto sound=document(processor);std::uint64_t revision=0;CHECK(processor.trySoundSnapshot(sound,revision));CHECK(processor.applyRackOperation(op,revision)==airack::RackCandidateStatus::ready);}
        const auto& model=*airack::studio::studioSkin();airack::RackPanelComponent drawer(processor,model);airack::studio::HouseUnit house(processor,model,{});airack::studio::EmptyRackSpace space(model);airack::RackStack stack(processor,model,house,space,drawer);stack.setSize(1547,754);stack.refresh(document(processor));
        const auto seen=[&](std::size_t count,const juce::String& preview,bool updated){const std::lock_guard lock(records->mutex);if(records->values.empty())return false;const auto& record=records->values.back();if(record.used.size()!=count||record.requested.size()!=count+(preview.isNotEmpty()?1:0)||bool(record.preview)!=preview.isNotEmpty())return false;if(record.preview&&record.preview->design.name!=preview)return false;if(updated&&record.used.front().design.name!="used-updated")return false;return std::none_of(record.used.begin(),record.used.end(),[](const auto& rack){return rack.design.name.startsWith("preview-");});};
        CHECK(wait("twelve used preparation",[&]{return seen(12,{},false);}));
        auto preview=airack::rackPreviewInstance(source);preview.design.id=juce::Uuid();preview.design.name="preview-A";airack::StackPlacement placement{{airack::RackRole::fx,airack::masterChain,4},"PREVIEW",preview,airack::rackPreviewValues(preview.design)};
        stack.setPlacement(placement);stack.refresh(document(processor));CHECK(wait("twelve used plus explicit preview",[&]{return seen(12,"preview-A",false);}));
        auto changed=document(processor);airack::RackOperation change;change.kind=airack::RackOperationKind::face;change.selected=changed.personalities->instances.front().id;change.design=changed.personalities->instances.front().design;change.design->name="used-updated";std::uint64_t changedRevision=0;CHECK(processor.trySoundSnapshot(changed,changedRevision));CHECK(processor.applyRackOperation(change,changedRevision)==airack::RackCandidateStatus::ready);stack.refresh(document(processor));CHECK(wait("changed used definition with full preview",[&]{return seen(12,"preview-A",true);}));
        placement.preview->design.name="preview-B";stack.setPlacement(placement);stack.refresh(document(processor));CHECK(wait("preview replacement",[&]{return seen(12,"preview-B",true);}));stack.setPlacement({});CHECK(wait("preview clear keeps used",[&]{return seen(12,{},true);}));
        airack::RackOperation remove;remove.kind=airack::RackOperationKind::remove;remove.selected=document(processor).personalities->instances.back().id;auto sound=document(processor);std::uint64_t revision=0;CHECK(processor.trySoundSnapshot(sound,revision));CHECK(processor.applyRackOperation(remove,revision)==airack::RackCandidateStatus::ready);stack.refresh(document(processor));placement.preview->design.name="preview-C";stack.setPlacement(placement);stack.refresh(document(processor));CHECK(wait("eleven used plus preview classification",[&]{return seen(11,"preview-C",true);}));stack.setPlacement({});CHECK(wait("eleven used preview clear",[&]{return seen(11,{},true);}));
        {const std::lock_guard lock(records->mutex);CHECK(std::all_of(records->values.begin(),records->values.end(),[](const auto& record){return record.used.size()<=12&&record.requested.size()<=13;}));std::cout<<"actual_stack_observations="<<records->values.size()<<" last_used="<<records->values.back().used.size()<<" last_preview="<<bool(records->values.back().preview)<<'\n';}
    }
    airack::setRackLibrarySkinTestHook({});runtime.reset();CHECK(requests==0);CHECK(!root.exists());
}
void stackLibraryPreparationCoalesces(){
    std::cout<<"SECTION stack_library_preparation_repeated_real_refresh\n";
    struct Outcome {unsigned attempt;juce::String used,preview;bool published;};
    struct Probe {std::mutex mutex;std::vector<Outcome> outcomes;unsigned begins=0,current=0;juce::String used,preview;std::atomic<unsigned> gated{0};std::atomic<bool> armed{false},released{true},timedOut{false};};
    const auto probe=std::make_shared<Probe>();
    airack::setRackLibrarySkinTestHook([probe](auto used,auto preview,auto,bool completed,bool published){bool hold=false;{
        const std::lock_guard lock(probe->mutex);const auto usedName=used.empty()?juce::String():used.front().design.name,previewName=preview?preview->design.name:juce::String();
        if(completed){probe->outcomes.push_back({probe->current,usedName,previewName,published});return;}
        probe->current=++probe->begins;if(probe->armed&&usedName==probe->used&&previewName==probe->preview){probe->armed=false;probe->gated=probe->current;hold=true;}
    }if(hold){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!probe->released&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));if(!probe->released)probe->timedOut=true;}});
    const auto root=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/rack-library-native/task5/fix2/ui-"+juce::Uuid().toString());auto runtime=std::make_shared<airack::RackLibraryRuntime>();runtime->store=std::make_shared<airack::RackPackageStore>(airack::RackLibraryPaths{root});std::atomic<int> requests{0};runtime->client=std::make_shared<airack::RackLibraryClient>(runtime->store,[&](const auto&,const auto&,const auto&,auto&){++requests;return airack::RackLibraryStatus::networkFailure;});
    const auto arm=[&](const juce::String& used,const juce::String& preview){const std::lock_guard lock(probe->mutex);probe->used=used;probe->preview=preview;probe->gated=0;probe->released=false;probe->armed=true;};
    const auto began=[&]{CHECK(wait("ordinary preparation held at worker boundary",[&]{return probe->gated.load()!=0;}));return probe->gated.load();};
    const auto release=[&](unsigned attempt,bool published){probe->released=true;CHECK(wait("held work completes",[&]{const std::lock_guard lock(probe->mutex);return std::any_of(probe->outcomes.begin(),probe->outcomes.end(),[&](const auto& outcome){return outcome.attempt==attempt;});}));const std::lock_guard lock(probe->mutex);const auto found=std::find_if(probe->outcomes.begin(),probe->outcomes.end(),[&](const auto& outcome){return outcome.attempt==attempt;});CHECK(found!=probe->outcomes.end()&&found->published==published);};
    const auto published=[&](const juce::String& used,const juce::String& preview){return wait("new desired work publishes",[&]{const std::lock_guard lock(probe->mutex);return std::any_of(probe->outcomes.begin(),probe->outcomes.end(),[&](const auto& outcome){return outcome.used==used&&outcome.preview==preview&&outcome.published;});});};
    {
        airack::InstrumentProcessor processor(runtime);CHECK(blankInstance(processor));const auto source=airack::test::frozenFactoryRackDesigns().front();airack::RackOperation op;op.design=source;op.design->id=juce::Uuid();op.design->name="used-old";op.newInstance=juce::Uuid();op.allocation={{0,0}};op.allowSlotReuse=true;auto sound=document(processor);std::uint64_t revision=0;CHECK(processor.trySoundSnapshot(sound,revision));CHECK(processor.applyRackOperation(op,revision)==airack::RackCandidateStatus::ready);
        const auto& model=*airack::studio::studioSkin();airack::RackPanelComponent drawer(processor,model);airack::studio::HouseUnit house(processor,model,{});airack::studio::EmptyRackSpace space(model);airack::RackStack stack(processor,model,house,space,drawer);stack.setSize(1547,754);stack.refresh(document(processor));CHECK(published("used-old",{}));
        auto preview=airack::rackPreviewInstance(airack::test::frozenExampleRackDesigns()[0]);airack::StackPlacement placement{{airack::RackRole::synth,airack::masterChain,1},"PREVIEW",preview,airack::rackPreviewValues(preview.design)};
        arm("used-old",preview.design.name);stack.setPlacement(placement);const auto stable=began();for(unsigned tick=0;tick<6;++tick){pump(20);stack.refresh(document(processor));if(tick==2){placement.destination.position=0;placement.letters="MOVED PREVIEW";}stack.setPlacement(placement);}release(stable,true);pump(40);{const std::lock_guard lock(probe->mutex);CHECK(probe->begins==stable);}CHECK(processor.preparedMakerSkin(preview)!=nullptr);
        unsigned readyBegins=0;{const std::lock_guard lock(probe->mutex);readyBegins=probe->begins;}for(unsigned tick=0;tick<4;++tick){pump(20);stack.refresh(document(processor));stack.setPlacement(placement);}pump(20);{const std::lock_guard lock(probe->mutex);CHECK(probe->begins==readyBegins);}
        const auto glass=airack::rackPreviewInstance(airack::test::frozenExampleRackDesigns()[1]);arm("used-old",glass.design.name);placement.preview=glass;placement.values=airack::rackPreviewValues(glass.design);stack.setPlacement(placement);const auto usedChange=began();sound=document(processor);airack::RackOperation change;change.kind=airack::RackOperationKind::face;change.selected=sound.personalities->instances.front().id;change.design=sound.personalities->instances.front().design;change.design->name="used-new";CHECK(processor.trySoundSnapshot(sound,revision));CHECK(processor.applyRackOperation(change,revision)==airack::RackCandidateStatus::ready);stack.refresh(document(processor));release(usedChange,false);CHECK(published("used-new",glass.design.name));
        arm("used-new",preview.design.name);placement.preview=preview;placement.values=airack::rackPreviewValues(preview.design);stack.setPlacement(placement);const auto replaced=began();placement.preview=glass;placement.values=airack::rackPreviewValues(glass.design);stack.setPlacement(placement);release(replaced,false);CHECK(published("used-new",glass.design.name));
        arm("used-new",preview.design.name);placement.preview=preview;placement.values=airack::rackPreviewValues(preview.design);stack.setPlacement(placement);const auto cleared=began();stack.setPlacement({});release(cleared,false);CHECK(published("used-new",{}));CHECK(!probe->timedOut);std::cout<<"unchanged_refresh_intervals=6 ready_refresh_intervals=4 changed_used_cancelled=1 replacement_cancelled=1 clear_cancelled=1\n";
    }
    probe->released=true;airack::setRackLibrarySkinTestHook({});runtime.reset();CHECK(requests==0);CHECK(!root.exists());
}
int main(int argc,char** argv)
{
    std::cout.setf(std::ios::unitbuf);
    // Detect listing without touching the GUI or filesystem; option values are not flags.
    for(int i=1;i<argc;++i)
    {
        const juce::String flag(argv[i]);
        if(flag=="--list-sections")listSections=true;
        else if((flag=="--images"||flag=="--only"||flag=="--partition")&&i+1<argc)++i;
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> gui;
    std::shared_ptr<PreparedTestMakerSkins> preparedSkins;
    if(!listSections){gui=std::make_unique<juce::ScopedJuceInitialiser_GUI>();preparedSkins=prepareTestMakerSkins();}
    // Reveals jump unless a section stands the motion's clock still and turns reduced motion off.
    if(!listSections)studio::motion::setForTests({},true);
    for(int i=1;i<argc;++i)
    {
        const juce::String flag(argv[i]);
        if(flag=="--list-sections")continue;
        if(i+1>=argc||(flag!="--images"&&flag!="--only"&&flag!="--partition"))return 2;
        const juce::String value(argv[++i]);
        if(flag=="--images"){if(!listSections){output=juce::File(value);CHECK(output.createDirectory());}}
        else if(flag=="--only")only.addTokens(value,",",{});
        else {if(value=="units")partition=1;else if(value=="stack")partition=2;else if(value=="window")partition=3;else return 2;}
    }
    if(!listSections)skinLoadsFromTheBinary();
    if(listSections||studio::studioSkin()!=nullptr)
    {
        RUN(stackLibraryPreparation);
        RUN(stackLibraryPreparationCoalesces);
        RUN(hitRegions);
        RUN(pressIsOneCommand);
        RUN(leverRules);
        RUN(tabOrder);
        RUN(lettering);
        RUN(overviewCells);
        RUN(roles);
        RUN(todayIds);
        RUN(sessionBranding);
        RUN(brandingWindowTitles);
        RUN(alphaBadge);
        RUN(manualScreenshots);
        RUN(feedbackPanel);
        RUN(libraryOfflineMessage);
#if defined(_WIN32) && defined(_DEBUG)
        RUN(unexpectedEndNoticeAppears);
#endif
        RUN(starterSound);
        RUN(builtInVoicesPlay);
        RUN(sessionDrawnState);
        RUN(sessionLoadError);
        RUN(sessionNoHostTempo);
        RUN(sessionMessages);
        RUN(sessionMessageOrder);
        RUN(sessionUnusualNames);
        RUN(sessionSoundName);
        RUN(sessionPathEntry);
        RUN(sessionTypedPath);
        RUN(sessionAiAndLever);
        RUN(sessionChooserOutlivesEditor);
        RUN(sessionKeepsTodaysSentences);
        RUN(sessionFirstSaveChooser);
        RUN(sessionFactorySounds);
        RUN(stripSettings);
        RUN(stripPlayback);
        RUN(stripExport);
        RUN(stripExportDrag);
        RUN(stripModeLever);
        RUN(stripOverview);
        RUN(stripNarrows);
        RUN(stripFold);
        RUN(patternElements);
        RUN(patternHitRegions);
        RUN(patternRoles);
        RUN(patternTabOrder);
        RUN(patternTodayIds);
        RUN(patternPathEntry);
        RUN(knobRules);
        RUN(noteTexts);
        RUN(rollDrawing);
        RUN(rollGestures);
        RUN(keyboardRules);
        RUN(patternSettingsKnobs);
        RUN(patternNoteKnobs);
        RUN(patternRoll);
        RUN(patternRollDrawing);
        RUN(patternKeyboard);
        RUN(patternPlayingLed);
        RUN(patternKnobGestures);
        RUN(patternRecallDuringGestures);
        RUN(patternExactEntry);
        RUN(patternFiles);
        RUN(patternExportDrag);
        RUN(patternChooserOutlivesUnit);
        RUN(patternMatchesPrototype);
        RUN(sideMatchesPrototype);
        RUN(programmerRow);
        RUN(programmerTitles);
        RUN(programmerButtons);
        RUN(programmerKnob);
        RUN(programmerFocus);
        RUN(programmerGlass);
        RUN(houseBranding);
        RUN(houseSkinLoads);
        RUN(houseNarrowSkin);
        RUN(houseNarrow);
        RUN(houseHitRegions);
        RUN(houseReadouts);
        RUN(houseKnobDrag);
        RUN(houseFaderDrag);
        RUN(houseButtons);
        RUN(houseButtonStates);
        RUN(houseKeys);
        RUN(houseAutomation);
        RUN(houseFold);
        RUN(houseRefused);
        RUN(houseClosing);
        RUN(housePlateIntact);
        RUN(emptySpace);
        RUN(houseAssign);
        RUN(houseSlotTapes);
        RUN(houseMacroDashes);
        RUN(houseAssignLocked);
        RUN(houseAssignEdits);
        RUN(houseTapeNames);
        RUN(tapeArt);
        RUN(houseAssignFollowsSound);
        RUN(houseAssignRetires);
        RUN(sideUnitsLoad);
        RUN(sideHitRegions);
        RUN(sidePresses);
        RUN(sideLeds);
        RUN(sideModelDisplay);
        RUN(sidePages);
        RUN(sideTextPlacement);
        RUN(sideRoles);
        RUN(sideTodayIds);
        RUN(racksPlacedNotMade);
        RUN(libraryOnePress);
        RUN(sideOptions);
        RUN(sidePromptAndContext);
        RUN(sideRows);
        RUN(sideActionButton);
        RUN(sideStatusTooltip);
        RUN(editorShowsTooltips);
        RUN(screenKeyboard);
        RUN(screenEntryTeardown);
        RUN(libraryWorkflow);
        RUN(libraryMatchesNativeReview);
        RUN(libraryRecall);
        RUN(libraryFrames);
        RUN(libraryPlaces);
        RUN(libraryKindNames);
        RUN(libraryFactoryDesigns);
        RUN(libraryPlacement);
        currentPartition=2;
        RUN(stack_new_instance);
        RUN(stackCharacterDrive);
        RUN(stackBandColor);
        RUN(stackSaturationCrossover);
        RUN(stackOrder);
        RUN(stackRails);
        RUN(stackFolds);
        RUN(stackBranding);
        RUN(stackMarks);
        RUN(stackSizes);
        RUN(stackSelection);
        RUN(stackDrawer);
        RUN(stack_drawer_controls);
        RUN(stack_generic_racks);
        RUN(stack_hidden_proposals);
        RUN(stackMoves);
        RUN(stackScrolls);
        RUN(stackTabOrder);
        RUN(stackPlacement);
        RUN(stackProposal);
        RUN(stackDrags);
        RUN(stackDragScrolls);
        RUN(stackFaceplate);
        RUN(stackUnitNarrow);
        RUN(stackNarrow);
        RUN(stackNarrowScroll);
        RUN(stackNarrowHouse);
        RUN(stackNarrowResize);
        RUN(stackNarrowDrag);
        RUN(stackNarrowMarks);
        RUN(stackNarrowRetire);
        RUN(stackFaceplateSwaps);
        RUN(stackFaceplateOperations);
        RUN(stackFaceplatePresses);
        RUN(stackFaceplateFold);
        RUN(stackFaceplateKeep);
        RUN(stackFaceplateMarks);
        RUN(stackFaceplatePreview);
        RUN(stackEmber);
        RUN(stackEmberDrawer);
        RUN(stackEmberKeep);
        RUN(stackEmberMarks);
        RUN(stackEmberPreview);
        RUN(stackGlass);
        RUN(stackGlassDoor);
        RUN(stackMotion);
        RUN(stackMotionFold);
        RUN(stackMotionRetire);
        RUN(stackMotionSnapshots);
        RUN(stackGlassStrip);
        RUN(stackGlassKeep);
        RUN(stackGlassPreview);
        RUN(stackOxide);
        RUN(stackOxideTempo);
        RUN(stackOxideKeep);
        RUN(stackOxideMarks);
        RUN(stackNight);
        RUN(stackNightDeck);
        RUN(stackNightKeep);
        RUN(stackNightMarks);
        RUN(stackNightPreview);
        RUN(stackTileArrayDrawer);
        RUN(stackLumen);
        RUN(stackLumenProposal);
        RUN(stackLumenLifetime);
        RUN(stackSmallHours);
        RUN(stackSmallHoursLid);
        RUN(stackSmallHoursMarks);
        RUN(stackSmallHoursPreview);
        currentPartition=3;
        RUN(windowDefault);
        RUN(windowInterlocks);
        RUN(windowPatternUnfolds);
        RUN(windowResizes);
        RUN(windowRendersAtTwiceScale);
        RUN(windowPatternAtTwiceScale);
        RUN(windowFocusOrder);
        RUN(windowProgrammer);
        RUN(windowProgrammerTabOrder);
        RUN(windowLibrary);
        RUN(windowEmptySpace);
        RUN(windowRackPanelShows);
        RUN(windowFirstRack);
        RUN(windowHouseFold);
        RUN(windowHouseNarrows);
        RUN(windowSideScale);
        RUN(windowSideSwitch);
        RUN(tileAiManualStatus);
        RUN(tilePopupEditorRetirement);
        RUN(windowUnitRetires);
        RUN(windowMakerFaceRetiresIdle);
        RUN(windowMakerFaceRetiresDragging);
        RUN(windowMakerUnitRetiresIdle);
        RUN(windowMakerUnitRetiresDragging);
        RUN(windowDrawerViews);
        RUN(windowStackIdles);
        RUN(windowLibraryPreview);
        RUN(windowLibraryStackIdles);
        RUN(sideStageLayout);
        RUN(sideSwaps);
        RUN(sideTabOrder);
        RUN(sideSeam);
        RUN(windowAssignRecall);
        RUN(windowWithoutSkin);
        RUN(houseRenders);
        RUN(reviewSnapshots);
        RUN(sideSnapshots);
        RUN(postedClickBehindSlowMessage);
        // Keep terminal dispatch rejection after every case that needs to pump the queue.
        RUN(stackRetirementRejectedPosts);
    }
    if(listSections)return 0;
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    if(failures==0)std::cout<<"ALL PASS\n";
    return failures?1:0;
}
