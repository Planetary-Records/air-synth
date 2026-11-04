#include "OwnedFixture.h"
#include "InstrumentProcessor.h"
#include "BlankInstance.h"
#include "PatternIO.h"
#include "PatternControls.h"
#include "PatternStrip.h"
#include "PatternUnit.h"
#include "SkinControls.h"
#include "PostedClick.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <iostream>
#include <chrono>
#include <cmath>
#include <windows.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

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
void click(juce::Button& button) { CHECK(clickPosted(button)); pump(); }
void click(juce::Component& root,const char* id)
{ if(auto* c=widget<juce::Button>(root,id)) click(*c); }
// A knob takes a typed value in its entry, which Enter opens and commits; the pattern path is the settings display's entry.
void text(juce::Component& root,const char* id,const juce::String& value)
{
    if(auto* k=dynamic_cast<studio::Knob*>(find(root,id)))
    {
        k->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        k->entry.setText(value,false);
        k->entry.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        pump();
    }
    else if(auto* c=widget<juce::TextEditor>(root,id)) { c->setText(value,false); if(c->onReturnKey)c->onReturnKey(); pump(); }
}
// METER's denominator is an item of its menu, GRID takes the grid's name and the mode is the lever's position.
void choice(juce::Component& root,const char* id,int value)
{
    const juce::String which(id);
    if(which=="pattern.meter.denominator")
    {
        bool chosen=false;
        if(auto* meter=widget<studio::Knob>(root,"pattern.meter.numerator"))
            for(auto& [name,act]:meter->menu())if(name=="Denominator "+juce::String(value)){act();chosen=true;}
        CHECK(chosen);pump();
    }
    else if(which=="pattern.grid")text(root,id,studio::gridNames[value-1]);
    else if(auto* lever=widget<studio::Lever>(root,id)){lever->set(value-1);pump();}
}
// The window's fold and displays are drawn controls, not juce::Buttons: press them as assistive technology does.
void press(juce::Component& root,const char* id)
{ auto* c=widget<juce::Component>(root,id);auto* h=c!=nullptr?c->getAccessibilityHandler():nullptr;CHECK(h!=nullptr&&h->getActions().invoke(juce::AccessibilityActionType::press));pump(); }
juce::String readValue(juce::Component& c)
{ auto* h=c.getAccessibilityHandler();return h!=nullptr&&h->getValueInterface()!=nullptr?h->getValueInterface()->getCurrentValueAsString():juce::String(); }
ProjectDocument snapshot(InstrumentProcessor& p)
{ ProjectDocument d; std::uint64_t r; CHECK(p.tryProjectSnapshot(d,r)); return d; }
std::uint64_t revision(InstrumentProcessor& p)
{ ProjectDocument d; std::uint64_t r=0; CHECK(p.tryProjectSnapshot(d,r)); return r; }
void pattern(InstrumentProcessor& p,const PatternDocument& d)
{ CHECK(p.applyPatternEdit(d,revision(p))); pump(); }
bool shows(const studio::Roll& roll,const PatternDocument& d)
{
    const auto& notes=roll.shown().notes;
    if(notes.size()!=d.noteCount)return false;
    for(std::size_t i=0;i<notes.size();++i)
    {
        const auto& n=d.notes[i];
        if(notes[i]!=studio::Note{n.startBeats,n.durationBeats,n.pitch,n.velocity,n.channel})return false;
    }
    return true;
}
// The editor shows a processor-side change on its next timer refresh, and the unit refuses an edit made from an older
// display, so the next action waits for the roll to show the change.
void pattern(InstrumentProcessor& p,const PatternDocument& d,const studio::Roll& roll)
{ CHECK(p.applyPatternEdit(d,revision(p))); CHECK(wait("pattern shown",[&]{return shows(roll,d);})); }
// After an undo and a redo the roll's notes can already match, so an action after a redo waits for the unit's revision.
void redo(InstrumentProcessor& p,juce::Component& editor)
{
    CHECK(p.redoSoundEdit());
    auto* unit=widget<studio::PatternUnit>(editor,"pattern.panel");
    CHECK(unit!=nullptr&&wait("redo shown",[&]{ProjectDocument d;std::uint64_t r=0;return p.tryProjectSnapshot(d,r)&&unit->revision()==r;}));
}
void recall(InstrumentProcessor& p,const ProjectDocument& d)
{ juce::MemoryBlock b; CHECK(encodeProjectState(d,b)); CHECK(p.restoreProjectState(b.getData(),(int)b.getSize())); pump(); }
juce::MouseEvent mouse(juce::Component& c,juce::Point<float> at,juce::Point<float> start,int modifiers=juce::ModifierKeys::leftButtonModifier,int clicks=1)
{ return {juce::Desktop::getInstance().getMainMouseSource(),at,juce::ModifierKeys(modifiers),1,0,0,0,0,&c,&c,juce::Time::getCurrentTime(),start,juce::Time::getCurrentTime(),clicks,at!=start}; }
// The roll's geometry is the view's; its events take points in its own coordinates.
juce::Point<float> cell(const studio::Roll& roll,double beat,int pitch) { return roll.at(beat,pitch)-roll.getPosition().toFloat(); }
// A note's bar as drawn: its centre, or just inside its right end where a drag resizes it.
juce::Point<float> bar(const studio::Roll& roll,int index,bool end=false)
{
    CHECK(index<(int)roll.shown().notes.size());
    if(index>=(int)roll.shown().notes.size())return {};
    const auto r=roll.noteRect(index);return juce::Point<float>{end?r.getRight()-2.0f:r.getCentreX(),r.getCentreY()}-roll.getPosition().toFloat();
}
juce::Point<float> step(const studio::Roll& roll,double beats,int pitches) { return {(float)beats*roll.beatWidth(),-(float)pitches*roll.rowHeight()}; }
void doubleClick(studio::Roll& roll,double beat,int pitch)
{ auto at=cell(roll,beat,pitch); roll.mouseDoubleClick(mouse(roll,at,at,juce::ModifierKeys::leftButtonModifier,2)); pump(); }
void select(studio::Roll& roll,juce::Point<float> at)
{ roll.mouseDown(mouse(roll,at,at)); roll.mouseUp(mouse(roll,at,at)); pump(); }
void select(studio::Roll& roll,double beat,int pitch) { select(roll,cell(roll,beat,pitch)); }
void drag(studio::Roll& roll,juce::Point<float> from,juce::Point<float> to,int modifiers=juce::ModifierKeys::leftButtonModifier)
{ roll.mouseDown(mouse(roll,from,from,modifiers)); roll.mouseDrag(mouse(roll,to,from,modifiers)); roll.mouseUp(mouse(roll,to,from,modifiers)); pump(); }
void expectNote(InstrumentProcessor& p,int index,PatternNote expected)
{ const auto d=snapshot(p).pattern; CHECK(index<d.noteCount); if(index<d.noteCount) CHECK(d.notes[(std::size_t)index]==expected); }
void image(juce::Component& editor,const char* name)
{
    if(output==juce::File{})return;
    auto file=output.getChildFile(juce::String(name)+".png"); auto stream=file.createOutputStream();
    if(stream){CHECK(stream->setPosition(0));CHECK(stream->truncate().wasOk());}
    auto picture=editor.createComponentSnapshot(editor.getLocalBounds(),true);
    CHECK(stream&&picture.isValid()); if(stream) CHECK(juce::PNGImageFormat().writeImageToStream(picture,*stream));
}
struct Host final:juce::AudioPlayHead
{
    int numerator=7,denominator=8; bool available=true; double ppq=0;
    juce::Optional<PositionInfo> getPosition() const override
    { if(!available)return {}; PositionInfo p;p.setTimeSignature(TimeSignature{numerator,denominator});p.setIsPlaying(true);p.setBpm(120);p.setPpqPosition(ppq);return p; }
};
void block(InstrumentProcessor& p,juce::AudioBuffer<float>& audio)
{ juce::MidiBuffer m;p.processBlock(audio,m); }
// Drive the actual same-process Windows native chooser. No production test hook,
// fake chooser, callback replacement, or external process interaction.
HWND dialogWindow(const wchar_t* action=L"Pattern Browse")
{
    struct Search { const wchar_t* title; HWND result=nullptr; } search {action};
    EnumWindows([](HWND w,LPARAM arg)->BOOL {
        DWORD pid=0;
        GetWindowThreadProcessId(w,&pid);
        if(pid!=GetCurrentProcessId())return TRUE;
        wchar_t cls[64]{},title[256]{};
        GetClassNameW(w,cls,64);
        GetWindowTextW(w,title,256);
        auto& search=*reinterpret_cast<Search*>(arg);
        if(wcscmp(cls,L"#32770")==0 && wcscmp(title,search.title)==0 && IsWindowVisible(w))
        {
            search.result=w;
            return FALSE;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&search)); return search.result;
}
struct DialogControls { HWND edit=nullptr,accept=nullptr,cancel=nullptr;int filenameMatches=0; };
DialogControls dialogControls(HWND dialog)
{
    DialogControls controls;
    if(!dialog)return controls;
    controls.accept=GetDlgItem(dialog,IDOK);controls.cancel=GetDlgItem(dialog,IDCANCEL);
    EnumChildWindows(dialog,[](HWND w,LPARAM arg)->BOOL {
        wchar_t cls[64]{},parent[64]{};GetClassNameW(w,cls,64);GetClassNameW(GetParent(w),parent,64);
        const int id=GetDlgCtrlID(w);auto& controls=*reinterpret_cast<DialogControls*>(arg);
        if(wcscmp(cls,L"Edit")==0 && IsWindowVisible(w) && IsWindowEnabled(w) && wcscmp(parent,L"ComboBox")==0 && (id==1001||id==1148)) {controls.edit=w;++controls.filenameMatches;}
        return TRUE;
    },reinterpret_cast<LPARAM>(&controls));return controls;
}
void dialogDiagnostic(const wchar_t* action)
{
    const auto dialog=dialogWindow(action);std::cout<<"Owned dialog diagnostic action="<<juce::String(action)<<" visible="<<(dialog!=nullptr)<<'\n';
    if(!dialog)return;
    EnumChildWindows(dialog,[](HWND child,LPARAM)->BOOL {
        wchar_t cls[64]{},parent[64]{},value[1024]{};GetClassNameW(child,cls,64);GetClassNameW(GetParent(child),parent,64);
        if(wcscmp(cls,L"Edit")!=0&&wcscmp(cls,L"Button")!=0)return TRUE;
        SendMessageW(child,WM_GETTEXT,1024,reinterpret_cast<LPARAM>(value));
        std::cout<<" control="<<juce::String(cls)<<" parent="<<juce::String(parent)<<" id="<<GetDlgCtrlID(child)<<" visible="<<IsWindowVisible(child)<<" enabled="<<IsWindowEnabled(child)<<" value="<<juce::String(value)<<'\n';return TRUE;
    },0);
}
bool answerDialog(const juce::File& file,const wchar_t* action,bool cancel=false)
{
    HWND dialog=nullptr;DialogControls controls;
    const auto ready=[&] {
        dialog=dialogWindow(action);controls=dialogControls(dialog);
        const auto button=cancel?controls.cancel:controls.accept;
        return dialog&&controls.filenameMatches==1&&button&&IsWindowVisible(button)&&IsWindowEnabled(button);
    };
    if(!wait("exact native FileChooser filename and action button ready",ready)){dialogDiagnostic(action);if(dialog)PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);return false;}
    if(!cancel)
    {
        const auto expected=file.getFullPathName();wchar_t actual[1024]{};
        SendMessageW(controls.edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(expected.toWideCharPointer()));
        SendMessageW(controls.edit,WM_GETTEXT,1024,reinterpret_cast<LPARAM>(actual));
        if(juce::String(actual)!=expected){dialogDiagnostic(action);PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);return false;}
    }
    PostMessageW(dialog,WM_COMMAND,MAKEWPARAM(cancel?IDCANCEL:IDOK,BN_CLICKED),reinterpret_cast<LPARAM>(cancel?controls.cancel:controls.accept));
    const bool closed=wait("owned native FileChooser closure",[&]{return !IsWindow(dialog)||!IsWindowVisible(dialog);});
    if(!closed){dialogDiagnostic(action);PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);}
    return closed;
}
void rawMidi(const juce::File& file)
{
    juce::MemoryBlock bytes;CHECK(file.loadFileAsData(bytes));
    CHECK(bytes.getSize()>30);if(bytes.getSize()<30)return;
    const auto* b=(const unsigned char*)bytes.getData();
    CHECK(std::string((const char*)b,4)=="MThd"); CHECK(b[8]==0&&b[9]==0&&b[10]==0&&b[11]==1&&b[12]==3&&b[13]==192);
    CHECK(std::string((const char*)b+14,4)=="MTrk");
    // Independently walk physical SMF bytes; preserve equal-tick off/on order.
    std::size_t at=22;std::vector<std::array<int,4>> events;int tick=0;
    auto variable=[&] { int n=0;for(int k=0;k<4&&at<bytes.getSize();++k){int v=b[at++];n=(n<<7)|(v&127);if(!(v&128))break;}return n; };
    while(at<bytes.getSize()) {
        tick+=variable();if(at>=bytes.getSize())break;int status=b[at++];
        if(status==255){if(at>=bytes.getSize())break;++at;at+=(std::size_t)variable();}
        else if((status&240)==128||(status&240)==144){CHECK(at+1<bytes.getSize());if(at+1>=bytes.getSize())break;events.push_back({tick,status,b[at],b[at+1]});at+=2;}
        else {CHECK(false);break;}
    }
    const std::vector<std::array<int,4>> expected{{0,144,60,100},{240,128,60,0},{240,144,60,80},{480,128,60,0}};
    CHECK(events==expected);
}
void transactionsAndFiles()
{
    std::cout<<"SECTION actual_piano_roll_transactions_and_files\n";
    auto processor = std::make_unique<InstrumentProcessor>(); auto& p = *processor;std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump();press(*editor,"strip.fold");
    // Keep repeated full-project return buffers off the piano-roll test stack.
    const auto readSnapshot=[&]{return airack::testing::fixtureValue([&]{return snapshot(p);});};
    auto* roll=widget<studio::Roll>(*editor,"pattern.roll");if(!roll)return;
    const int altDrag=juce::ModifierKeys::leftButtonModifier|juce::ModifierKeys::altModifier;
    CHECK((*readSnapshot()).mode==PlaybackMode::externalMidi);
    doubleClick(*roll,0,60);expectNote(p,0,{0,.25,60,100,1});CHECK(p.undoSoundEdit());CHECK((*readSnapshot()).pattern.noteCount==0);redo(p,*editor);
    drag(*roll,cell(*roll,.05,60),cell(*roll,.55,62));expectNote(p,0,{.5,.25,62,100,1});CHECK(p.undoSoundEdit());expectNote(p,0,{0,.25,60,100,1});redo(p,*editor);
    drag(*roll,bar(*roll,0,true),bar(*roll,0,true)+step(*roll,.5,0));expectNote(p,0,{.5,.75,62,100,1});CHECK(p.undoSoundEdit());expectNote(p,0,{.5,.25,62,100,1});redo(p,*editor);
    drag(*roll,cell(*roll,.55,62),cell(*roll,.55,62).translated(0,-27),altDrag);expectNote(p,0,{.5,.75,62,127,1});
    auto unchangedOwner=readSnapshot();auto& unchanged=*unchangedOwner;auto r=revision(p);auto from=cell(*roll,.55,62);roll->mouseDown(mouse(*roll,from,from));roll->mouseDrag(mouse(*roll,cell(*roll,1.05,65),from));CHECK((*readSnapshot())==unchanged);roll->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));roll->mouseUp(mouse(*roll,cell(*roll,1.05,65),from));CHECK((*readSnapshot())==unchanged);CHECK(revision(p)==r);
    roll->mouseDown(mouse(*roll,from,from));roll->mouseDrag(mouse(*roll,cell(*roll,1.05,65),from));auto recalled=unchanged;recalled.pattern.notes[0].pitch=80;recall(p,recalled);roll->mouseUp(mouse(*roll,cell(*roll,1.05,65),from));pump();CHECK((*readSnapshot())==recalled);
    auto* status=widget<juce::Label>(*editor,"pattern.status");CHECK(status&&status->getText().containsIgnoreCase("changed"));
    // Full recall clears selection without revealing the replacement pitch.
    // Make this explicit user selection visible before delivering its event.
    std::cout<<"STAGE reveal recalled pitch 80; prior top row "<<roll->topRow()<<'\n';
    CHECK(roll->topRow()<80);
    roll->reveal(80);
    CHECK(roll->topRow()>=80&&roll->topRow()-roll->rowsShown()<80);
    select(*roll,.55,80);
    text(*editor,"pattern.note.start","0.12345678901234566");text(*editor,"pattern.note.duration","0.00000012345678901234566");
    expectNote(p,0,{.12345678901234566,.00000012345678901234566,80,127,1});
    auto* start=widget<studio::Knob>(*editor,"pattern.note.start");auto* duration=widget<studio::Knob>(*editor,"pattern.note.duration");
    CHECK(start&&start->entry.getText().getDoubleValue()==.12345678901234566);CHECK(duration&&duration->entry.getText().getDoubleValue()==.00000012345678901234566);
    const auto exactOwner=readSnapshot();const auto& exact=*exactOwner;r=revision(p);pump(80);CHECK((*readSnapshot())==exact);CHECK(revision(p)==r);
    for(const auto bad:{".5trailing","nan","inf","","1 2"}) {text(*editor,"pattern.note.start",bad);CHECK((*readSnapshot())==exact);CHECK(revision(p)==r);CHECK(status->getText().containsIgnoreCase("invalid"));}
    text(*editor,"pattern.note.pitch","0");text(*editor,"pattern.note.velocity","1");text(*editor,"pattern.note.channel","16");expectNote(p,0,{.12345678901234566,.00000012345678901234566,0,1,16});
    text(*editor,"pattern.note.pitch","127");text(*editor,"pattern.note.velocity","127");text(*editor,"pattern.note.channel","1");expectNote(p,0,{.12345678901234566,.00000012345678901234566,127,127,1});
    auto rangeOwner=readSnapshot();auto& range=*rangeOwner;for(auto pair:{std::pair{"pattern.note.pitch","128"},{"pattern.note.velocity","0"},{"pattern.note.channel","17"},{"pattern.note.duration","0"},{"pattern.note.pitch","60.5"}}){text(*editor,pair.first,pair.second);CHECK((*readSnapshot())==range);}
    text(*editor,"pattern.bars","8");text(*editor,"pattern.meter.numerator","7");choice(*editor,"pattern.meter.denominator",8);choice(*editor,"pattern.grid",5);text(*editor,"pattern.swing","1");
    CHECK((*readSnapshot()).pattern.bars==8);CHECK(((*readSnapshot()).pattern.meter==PhraseMeter{7,8}));CHECK((*readSnapshot()).pattern.grid==PatternGrid::sixtyFourth);CHECK((*readSnapshot()).pattern.swing==1);
    text(*editor,"pattern.note.start","27");auto longPhraseOwner=readSnapshot();auto& longPhrase=*longPhraseOwner;text(*editor,"pattern.bars","1");CHECK((*readSnapshot())==longPhrase);text(*editor,"pattern.meter.numerator","1");CHECK((*readSnapshot())==longPhrase);
    for(int grid=1;grid<=5;++grid){choice(*editor,"pattern.grid",grid);CHECK((int)(*readSnapshot()).pattern.grid==grid-1);}
    choice(*editor,"pattern.mode",2);CHECK((*readSnapshot()).mode==PlaybackMode::internalPattern);CHECK(p.undoSoundEdit());CHECK((*readSnapshot()).mode==PlaybackMode::externalMidi);redo(p,*editor);
    click(*editor,"pattern.note.delete");CHECK((*readSnapshot()).pattern.noteCount==0);CHECK(p.undoSoundEdit());CHECK(wait("restored note shown",[&]{return roll->shown().notes.size()==1;}));select(*roll,bar(*roll,0));roll->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));CHECK((*readSnapshot()).pattern.noteCount==0);
    click(*editor,"pattern.note.add");CHECK((*readSnapshot()).pattern.noteCount==1);
    Host host;p.setPlayHead(&host);p.prepareToPlay(48000,256);juce::AudioBuffer<float> audio(2,256);block(p,audio);
    click(*editor,"pattern.new");CHECK(((*readSnapshot()).pattern.meter==PhraseMeter{7,8}));CHECK((*readSnapshot()).pattern.noteCount==0);
    host.numerator=3;host.denominator=4;block(p,audio);pump();CHECK(((*readSnapshot()).pattern.meter==PhraseMeter{7,8}));
    p.setPlayHead(nullptr);block(p,audio);click(*editor,"pattern.new");CHECK(((*readSnapshot()).pattern.meter==PhraseMeter{4,4}));CHECK(status->getText().contains("4/4"));
    PatternDocument dense;dense.bars=8;dense.noteCount=256;for(int i=0;i<256;++i)dense.notes[(std::size_t)i]={i*.125,.0625,(std::uint8_t)(i%128),100,1};pattern(p,dense,*roll);r=revision(p);roll->reveal(60);doubleClick(*roll,.06,60);CHECK((*readSnapshot()).pattern==dense);CHECK(revision(p)==r);CHECK(status->getText().contains("256"));
    // The roll shows the whole phrase across its grid and scrolls its rows over every pitch.
    for(int i=0;i<dense.noteCount;++i){const auto n=roll->noteRect(i);CHECK(n.getX()>=roll->xOf(0)-.5f&&n.getRight()<=roll->xOf(patternLengthBeats(dense))+.5f);}
    roll->scrollTo(127);CHECK(roll->topRow()==127);image(*editor,"dense-scroll");roll->scrollTo(0);CHECK(roll->topRow()==roll->rowsShown()-1);
    PatternDocument filePattern;filePattern.noteCount=2;filePattern.notes[0]={0,.25,60,100,1};filePattern.notes[1]={.25,.25,60,80,1};pattern(p,filePattern,*roll);
    const auto folder=juce::File(RACKSYNTH_WORKSPACE).getChildFile(".superpowers/sdd/2026-10-04-m3-project-pattern-editor").getNonexistentChildFile("task-3-files","",false);CHECK(folder.createDirectory());
    const auto asset=folder.getChildFile("exact.aipattern"),midi=folder.getChildFile("exact.mid");
    text(*editor,"pattern.path",asset.getFullPathName());click(*editor,"pattern.save");PatternDocument disk;CHECK(loadPatternAsset(asset,disk));CHECK(disk==filePattern);
    auto destinationOwner=readSnapshot();auto& destination=*destinationOwner;auto changed=destination.pattern;changed.notes[0].pitch=80;pattern(p,changed,*roll);text(*editor,"pattern.path",asset.getFullPathName());click(*editor,"pattern.load");CHECK((*readSnapshot()).pattern==filePattern);CHECK((*readSnapshot()).sound==destination.sound);CHECK((*readSnapshot()).mode==destination.mode);CHECK(p.undoSoundEdit());CHECK((*readSnapshot()).pattern==changed);redo(p,*editor);
    text(*editor,"pattern.path",midi.getFullPathName());click(*editor,"pattern.export");rawMidi(midi);
    const auto stableOwner=readSnapshot();const auto& stable=*stableOwner;r=revision(p);text(*editor,"pattern.path","relative.aipattern");click(*editor,"pattern.save");CHECK((*readSnapshot())==stable);CHECK(revision(p)==r);CHECK(status->getText().containsIgnoreCase("absolute"));
    text(*editor,"pattern.path",folder.getChildFile("missing.aipattern").getFullPathName());click(*editor,"pattern.load");CHECK((*readSnapshot())==stable);CHECK(revision(p)==r);
    // Snapshot at click precedes the actual native chooser callback and a later edit.
    text(*editor,"pattern.path","");click(*editor,"pattern.save");auto afterClick=filePattern;afterClick.notes[0].pitch=99;pattern(p,afterClick,*roll);const auto captured=folder.getChildFile("captured.aipattern");CHECK(answerDialog(captured,L"Pattern Save"));CHECK(wait("captured asset saved",[&]{return captured.existsAsFile();}));CHECK(loadPatternAsset(captured,disk));CHECK(disk==filePattern);CHECK((*readSnapshot()).pattern==afterClick);pump();
    pattern(p,filePattern,*roll);text(*editor,"pattern.path","");click(*editor,"pattern.export");pattern(p,afterClick,*roll);const auto capturedMidi=folder.getChildFile("captured.mid");CHECK(answerDialog(capturedMidi,L"Pattern Export"));CHECK(wait("captured MIDI saved",[&]{return capturedMidi.existsAsFile();}));rawMidi(capturedMidi);pump();
    r=revision(p);click(*editor,"pattern.browse");CHECK(answerDialog({},L"Pattern Browse",true));CHECK(wait("cancel callback",[&]{return status->getText().containsIgnoreCase("cancel");}));CHECK((*readSnapshot()).pattern==afterClick);CHECK(revision(p)==r);
    const juce::Component::SafePointer<juce::Component> safe(editor.get());click(*editor,"pattern.browse");CHECK(wait("chooser before editor teardown",[]{return dialogWindow()!=nullptr;}));editor.reset();CHECK(safe==nullptr);CHECK(wait("chooser teardown",[]{return dialogWindow()==nullptr;}));pump();
    CHECK(folder.deleteRecursively());
}
// I1 regressions use synchronous real callbacks so the 25ms timer cannot hide
// a recall between the displayed selection and the next user action.
void staleDisplayedIntent()
{
    std::cout<<"SECTION stale_displayed_intent_across_project_replacement\n";
    enum class Action { remove, numeric, drag, focusedRecall, sameValueRecall,
                        focusedExternalEdit, settingsBeforeTimer, focusedSettings };
    const std::array stages {
        Action::remove, Action::numeric, Action::drag, Action::focusedRecall,
        Action::sameValueRecall, Action::focusedExternalEdit,
        Action::settingsBeforeTimer, Action::focusedSettings
    };
    for(const auto action:stages)
    {
        std::cout<<"STAGE stale intent "<<(int)action<<'\n';
        auto processor = std::make_unique<InstrumentProcessor>(); auto& p = *processor;
        auto a=snapshot(p);
        a.pattern.noteCount=1;
        a.pattern.notes[0]={0,.75,60,100,1};
        recall(p,a);

        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
        editor->addToDesktop(0);
        editor->setVisible(true);
        editor->toFront(false);
        pump();
        press(*editor,"strip.fold");
        auto* roll=widget<studio::Roll>(*editor,"pattern.roll");
        auto* field=widget<studio::Knob>(*editor,"pattern.note.pitch");
        auto* swing=widget<studio::Knob>(*editor,"pattern.swing");
        auto* remove=widget<juce::Button>(*editor,"pattern.note.delete");
        auto* status=widget<juce::Label>(*editor,"pattern.status");
        if(!roll||!field||!swing||!remove||!status)return;
        select(*roll,.1,60);
        CHECK(field->isEnabled());
        CHECK(field->entry.getText()=="60");

        // The pending intent is a value typed in an open entry.
        const bool settings=action==Action::settingsBeforeTimer||action==Action::focusedSettings;
        auto* pending=settings?swing:field;
        const bool focused=action==Action::focusedRecall||action==Action::sameValueRecall
            ||action==Action::focusedExternalEdit||action==Action::focusedSettings;
        pending->openEntry();
        CHECK(pending->entry.isVisible());
        if(focused)
        {
            pending->entry.grabKeyboardFocus();
            CHECK(wait("owned pending entry has keyboard focus",[&]{return pending->entry.hasKeyboardFocus(false);}));
            CHECK(juce::Component::getCurrentlyFocusedComponent()==&pending->entry);
        }
        pending->entry.setText(settings?"0.75":"91",false);
        auto b=a;
        if(action!=Action::sameValueRecall)
        {
            b.pattern.notes[0]={.125,.5,61,75,2};
            b.pattern.swing=.25;
        }

        // Deliberately no helper pump between replacement and callback.
        const bool external=action==Action::focusedExternalEdit;
        if(external)
        {
            CHECK(p.applyPatternEdit(b.pattern,revision(p)));
        }
        else
        {
            juce::MemoryBlock bytes;
            CHECK(encodeProjectState(b,bytes));
            CHECK(p.restoreProjectState(bytes.getData(),(int)bytes.getSize()));
        }
        const auto recalledRevision=revision(p);
        if(focused)pump(80);

        if(action==Action::remove)
        {
            CHECK((bool)remove->onClick);
            remove->onClick();
        }
        else if(action==Action::drag)
        {
            const auto from=cell(*roll,.1,60),to=cell(*roll,.6,62);
            roll->mouseDown(mouse(*roll,from,from));
            roll->mouseDrag(mouse(*roll,to,from));
            roll->mouseUp(mouse(*roll,to,from));
        }
        else
        {
            CHECK((bool)pending->entry.onReturnKey);
            pending->entry.onReturnKey();
        }
        CHECK(snapshot(p)==b);
        CHECK(revision(p)==recalledRevision);
        CHECK(status->getText().containsIgnoreCase("changed")
            ||status->getText().containsIgnoreCase("cancel")
            ||status->getText().containsIgnoreCase("reject"));
        CHECK(!field->isEnabled());
        CHECK(field->entry.getText().isEmpty());
        CHECK(swing->entry.getText().getDoubleValue()==b.pattern.swing);

        std::cout<<"STAGE replacement history "<<(int)action<<'\n';
        if(external)
        {
            CHECK(p.undoSoundEdit());
            CHECK(snapshot(p)==a);
            CHECK(!p.undoSoundEdit());
            CHECK(p.redoSoundEdit());
            status->setText({},juce::dontSendNotification);
            const bool displayed=wait("fresh post-redo display acknowledgement",[&]{return status->getText().containsIgnoreCase("changed");});
            const auto afterRedo=snapshot(p);
            CHECK(displayed&&afterRedo==b);
            CHECK(!p.redoSoundEdit());
        }
        else
        {
            CHECK(!p.undoSoundEdit());
            CHECK(!p.redoSoundEdit());
        }
        pump();

        // Explicit selection admits new intent. Own sequential edits retain it.
        std::cout<<"STAGE explicit reselection and own edits "<<(int)action<<'\n';
        const auto& note=b.pattern.notes[0];
        select(*roll,note.startBeats+.1,note.pitch);
        text(*editor,"pattern.note.pitch","64");
        auto firstEdit=b;
        firstEdit.pattern.notes[0].pitch=64;
        CHECK(snapshot(p)==firstEdit);
        text(*editor,"pattern.note.velocity","88");
        auto secondEdit=firstEdit;
        secondEdit.pattern.notes[0].velocity=88;
        CHECK(snapshot(p)==secondEdit);
        CHECK(p.undoSoundEdit());
        CHECK(snapshot(p)==firstEdit);
        CHECK(p.redoSoundEdit());
        CHECK(wait("undo/redo displayed before next gesture",[&]{return status->getText().containsIgnoreCase("changed");}));
        const auto from=cell(*roll,note.startBeats+.1,64);
        drag(*roll,from,from+step(*roll,.25,1));
        auto moved=secondEdit;
        moved.pattern.notes[0].startBeats=std::round((note.startBeats+.25)/.25)*.25;
        moved.pattern.notes[0].pitch=65;
        CHECK(snapshot(p)==moved);
        CHECK(p.undoSoundEdit());
        CHECK(snapshot(p)==secondEdit);
        CHECK(wait("undo displayed before settings edit",[&]{return !field->isEnabled();}));
        text(*editor,"pattern.swing","0.5");
        auto settingsEdit=secondEdit;
        settingsEdit.pattern.swing=.5;
        CHECK(snapshot(p)==settingsEdit);
        CHECK(p.undoSoundEdit());
        CHECK(snapshot(p)==secondEdit);
    }
}
void stickyHeadersRejectHiddenNotes()
{
    std::cout<<"SECTION sticky_headers_reject_hidden_note_gestures\n";
    // A press on the ruler's keys at the note's row, or on the bar header where the note would be drawn with its row
    // scrolled just above the grid.
    enum class Kind { move, resize, velocity };
    struct Gesture
    {
        const char* name;
        bool header;
        Kind kind;
    };
    const std::array gestures {
        Gesture {"pitch ruler move",false,Kind::move},
        Gesture {"pitch ruler resize",false,Kind::resize},
        Gesture {"pitch ruler Alt velocity",false,Kind::velocity},
        Gesture {"bar header move",true,Kind::move},
        Gesture {"bar header resize",true,Kind::resize},
        Gesture {"bar header Alt velocity",true,Kind::velocity}
    };
    for(const auto& gesture:gestures)
    {
        std::cout<<"STAGE sticky header "<<gesture.name<<'\n';
        auto processor = std::make_unique<InstrumentProcessor>(); auto& p = *processor;
        auto base=snapshot(p);
        base.mode=PlaybackMode::internalPattern;
        base.pattern.bars=8;
        base.pattern.swing=.25;
        base.pattern.noteCount=1;
        base.pattern.notes[0]={2,1,60,80,7};
        recall(p,base);

        // Retain an undo image and a pending redo image. A hidden gesture must
        // neither append an edit nor discard the user's pending redo history.
        auto current=base;
        current.pattern.notes[0].velocity=90;
        pattern(p,current.pattern);
        auto future=current;
        future.pattern.notes[0].velocity=100;
        pattern(p,future.pattern);
        CHECK(p.undoSoundEdit());
        CHECK(snapshot(p)==current);

        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
        editor->addToDesktop(0);
        editor->setVisible(true);
        editor->toFront(false);
        pump();
        press(*editor,"strip.fold");
        auto* roll=widget<studio::Roll>(*editor,"pattern.roll");
        auto* field=widget<studio::Knob>(*editor,"pattern.note.pitch");
        if(!roll||!field)return;
        if(gesture.header)roll->scrollTo(59);
        // The note is beat 2..3 at MIDI pitch 60; a resize would take its last pixels.
        const auto row=roll->rowCentre(60),noteEnd=std::round(roll->xOf(3))-1.0f;
        const juce::Point<float> at {gesture.header?(gesture.kind==Kind::resize?noteEnd-2.0f:roll->xOf(2.5)):((float)roll->getX()+roll->xOf(0))/2.0f,row};
        CHECK(roll->getBounds().toFloat().contains(at));
        if(gesture.header)CHECK(roll->topRow()==59&&row<roll->rowCentre(59)-roll->rowHeight()/2.0f&&at.x>roll->xOf(2)&&at.x<noteEnd);
        else CHECK(roll->topRow()>=60&&roll->topRow()-roll->rowsShown()<60&&at.x<roll->xOf(0));
        CHECK(!field->isEnabled());
        const auto beforeRevision=revision(p);
        const int modifiers=juce::ModifierKeys::leftButtonModifier|(gesture.kind==Kind::velocity?juce::ModifierKeys::altModifier:0);
        const auto from=at-roll->getPosition().toFloat();
        const auto to=from+(gesture.kind==Kind::move?step(*roll,.25,1):gesture.kind==Kind::resize?step(*roll,.25,0):juce::Point<float>{0,-20});

        roll->mouseDown(mouse(*roll,from,from,modifiers));
        CHECK(snapshot(p)==current);
        CHECK(revision(p)==beforeRevision);
        CHECK(!field->isEnabled());
        roll->mouseDrag(mouse(*roll,to,from,modifiers));
        CHECK(snapshot(p)==current);
        CHECK(revision(p)==beforeRevision);
        roll->mouseUp(mouse(*roll,to,from,modifiers));
        pump();
        CHECK(snapshot(p)==current);
        CHECK(revision(p)==beforeRevision);
        CHECK(!field->isEnabled());

        std::cout<<"STAGE preserved history "<<gesture.name<<'\n';
        CHECK(p.undoSoundEdit());
        CHECK(snapshot(p)==base);
        CHECK(!p.undoSoundEdit());
        CHECK(p.redoSoundEdit());
        CHECK(snapshot(p)==current);
        CHECK(p.redoSoundEdit());
        CHECK(snapshot(p)==future);
        CHECK(!p.redoSoundEdit());
    }

    std::cout<<"STAGE visible note move and one-step undo redo\n";
    auto processor = std::make_unique<InstrumentProcessor>(); auto& p = *processor;
    auto before=snapshot(p);
    before.mode=PlaybackMode::internalPattern;
    before.pattern.bars=8;
    before.pattern.swing=.25;
    before.pattern.noteCount=1;
    before.pattern.notes[0]={2,1,60,90,7};
    recall(p,before);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    editor->addToDesktop(0);
    editor->setVisible(true);
    editor->toFront(false);
    pump();
    press(*editor,"strip.fold");
    auto* roll=widget<studio::Roll>(*editor,"pattern.roll");
    if(!roll)return;
    CHECK(roll->topRow()>=61&&roll->topRow()-roll->rowsShown()<60);
    const auto from=cell(*roll,2.25,60),to=from+step(*roll,.25,1);
    const auto beforeRevision=revision(p);
    roll->mouseDown(mouse(*roll,from,from));
    roll->mouseDrag(mouse(*roll,to,from));
    CHECK(snapshot(p)==before);
    CHECK(revision(p)==beforeRevision);
    roll->mouseUp(mouse(*roll,to,from));
    pump();
    auto moved=before;
    moved.pattern.notes[0]={2.25,1,61,90,7};
    CHECK(snapshot(p)==moved);
    CHECK(revision(p)==beforeRevision+1);
    CHECK(p.undoSoundEdit());
    CHECK(snapshot(p)==before);
    CHECK(!p.undoSoundEdit());
    CHECK(p.redoSoundEdit());
    CHECK(snapshot(p)==moved);
    CHECK(!p.redoSoundEdit());
}
void keyboardAndTeardown()
{
    std::cout<<"SECTION real_keyboard_and_teardown\n";
    auto processor = std::make_unique<InstrumentProcessor>(); auto& p = *processor;CHECK(copperArcInstance(p));p.prepareToPlay(48000,256);juce::AudioBuffer<float> audio(2,256);
    for(int cycle=0;cycle<6;++cycle)
    {
        auto mode=cycle%2?PlaybackMode::internalPattern:PlaybackMode::externalMidi;CHECK(p.setPlaybackMode(mode,revision(p)));
        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump();
        press(*editor,"strip.fold");
        auto* keys=widget<studio::Keyboard>(*editor,"pattern.keyboard");if(!keys)return;
        // The membrane hit-tests its drawn keys itself: MIDI 60 is key 12 from the default lowest key, 48.
        CHECK(keys->lowestKey()==48);
        const auto rect=keys->keyRect(12);const juce::Point<float> drawn{rect.getCentreX(),rect.getBottom()-4};
        CHECK(keys->pitchAt(drawn)==60);
        const auto at=drawn-keys->getPosition().toFloat();
        keys->mouseDown(mouse(*keys,at,at));block(p,audio);CHECK(audio.getMagnitude(0,256)>0);
        if(cycle==2){auto current=snapshot(p);recall(p,current);block(p,audio);CHECK(audio.getMagnitude(0,256)==0);keys->mouseUp(mouse(*keys,at,at));}
        else if(cycle==3) {editor.reset();}
        else {keys->mouseUp(mouse(*keys,at,at));}
        for(int n=0;n<350;++n)block(p,audio);CHECK(audio.getMagnitude(0,256)<1e-7f);
        if(editor){keys->mouseDown(mouse(*keys,at,at));keys->mouseUp(mouse(*keys,at,at));block(p,audio);for(int n=0;n<350;++n)block(p,audio);CHECK(audio.getMagnitude(0,256)<1e-7f);editor.reset();}
        block(p,audio);CHECK(audio.getMagnitude(0,256)<1e-7f);
    }
}
void layoutAndStatus()
{
    std::cout<<"SECTION layout_and_persistent_status\n";
    auto processor = std::make_unique<InstrumentProcessor>(); auto& p = *processor;p.prepareToPlay(48000,256);juce::AudioBuffer<float> audio(2,256);
    for(int pass=0;pass<2;++pass)
    {
        CHECK(!p.restoreProjectState(nullptr,0));std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());editor->addToDesktop(0);editor->setVisible(true);pump();
        if(!widget<juce::Component>(*editor,"pattern.panel"))return;
        press(*editor,"strip.fold");
        for(auto size:{juce::Point<int>{1080,680},juce::Point<int>{1600,1000}})
        {
            editor->setSize(size.x,size.y);pump();
            auto shown=[&](const char* id)
            {
                auto* c=widget<juce::Component>(*editor,id);if(!c)return;
                // The stage scales its units, so their edges may fall between the editor's pixels.
                const auto area=editor->getLocalArea(c,c->getLocalBounds().toFloat());
                const auto inside=editor->getLocalBounds().toFloat().expanded(0.5f).contains(area);
                CHECK(c->isShowing());CHECK(inside);CHECK(c->getWidth()>0&&c->getHeight()>0);
                if(!c->isShowing())std::cout<<"  "<<id<<" not showing at "<<size.x<<"x"<<size.y<<", pass "<<pass<<'\n';
                if(!inside)std::cout<<"  "<<id<<" at "<<area.toString()<<" outside "<<editor->getLocalBounds().toString()<<'\n';
            };
            for(const char* id:{"pattern.panel","pattern.roll","pattern.mode","pattern.keyboard","pattern.settings","session.display"})shown(id);
            // The settings display draws the pattern's status; its accessible value keeps the status line's text.
            if(auto* settings=widget<juce::Component>(*editor,"pattern.settings"))
                if(auto* line=widget<juce::Label>(*editor,"pattern.status"))CHECK(!line->getText().isEmpty()&&readValue(*settings).contains(line->getText()));
            // The session display draws the status, tempo and load-error lines; its accessible value keeps their text.
            if(auto* display=widget<juce::Component>(*editor,"session.display"))
                for(const char* id:{"state.load.error","tempo.status","status"})
                    if(auto* line=widget<juce::Label>(*editor,id))CHECK(readValue(*display).contains(line->getText()));
            press(*editor,"session.display");shown("preset.path");
            if(auto* entry=widget<juce::TextEditor>(*editor,"preset.path"))if(entry->onEscapeKey)entry->onEscapeKey();
            pump();
            auto* roll=widget<juce::Component>(*editor,"pattern.roll");CHECK(roll&&editor->getLocalArea(roll,roll->getLocalBounds()).getHeight()>=145);
            image(*editor,size.x==1080?"minimum-error":"large-error");
        }
        auto* error=widget<juce::Label>(*editor,"state.load.error");CHECK(error&&!error->getText().isEmpty());
        click(*editor,"pattern.new");CHECK(!error->getText().isEmpty());
        choice(*editor,"pattern.mode",2);block(p,audio);auto* status=widget<juce::Label>(*editor,"pattern.status");CHECK(wait("missing host timing status",[&]{return status->getText().containsIgnoreCase("waiting for host timing");}));image(*editor,"missing-timing");
        Host host;host.ppq=16777217.;p.setPlayHead(&host);block(p,audio);CHECK(wait("invalid coordinate status",[&]{return status->getText().contains("16777216");}));p.setPlayHead(nullptr);
        auto state=snapshot(p);recall(p,state);CHECK(wait("successful full recall clears warning",[&]{return error->getText().isEmpty();}));
    }
}
void postedClickBehindSlowMessage()
{
    std::cout<<"SECTION posted_click_behind_slow_message\n";
    CHECK(clickRunsBehindSlowMessage([](juce::Button& button) { click(button); }));
}
}
// --only layout,keyboard: only the sections whose functions' names start with one of these, for a quick local run.
juce::StringArray only;
int sections=0;
bool runs(const char* name)
{
    for(const auto& prefix:only)if(juce::String(name).startsWith(prefix))return true;
    return only.isEmpty();
}
#define RUN(section) if(runs(#section)){section();++sections;}
int main(int argc,char** argv)
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    juce::ScopedJuceInitialiser_GUI gui;
    for(int i=1;i<argc;i+=2)
    {
        const juce::String flag(argv[i]);
        if(i+1>=argc||(flag!="--images"&&flag!="--only"))return 2;
        if(flag=="--images"){output=juce::File(argv[i+1]);CHECK(output.createDirectory());}
        else only.addTokens(argv[i+1],",",{});
    }
    RUN(transactionsAndFiles);
    RUN(staleDisplayedIntent);
    RUN(stickyHeadersRejectHiddenNotes);
    RUN(keyboardAndTeardown);
    RUN(layoutAndStatus);
    RUN(postedClickBehindSlowMessage);
    std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";
    return failures?1:0;
}
