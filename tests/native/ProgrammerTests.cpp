#include "FrozenRackExamples.h"
#include "AllControlsPanel.h"
#include "ProgrammerValues.h"
#include "ProgrammerUnit.h"
#include "ParameterRegistry.h"
#include "InstrumentProcessor.h"
#include "BlankInstance.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "StudioSkin.h"
#include "UserReach.h"
#include "WheelLatch.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>
using namespace airack;
using namespace airack::studio;
namespace {
int checks=0,failures=0,sections=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<std::endl;} } while(false)
void section(const char* name){++sections;std::cout<<"SECTION "<<name<<'\n';}
// With --images DIR, the checks keep what they drew there as PNGs.
juce::File output;
void keep(const juce::String& name,const juce::Image& image)
{
    if(output==juce::File())return;
    const auto file=output.getChildFile(name+".png");
    file.deleteFile();
    juce::FileOutputStream out(file);
    CHECK(out.openedOk()&&juce::PNGImageFormat().writeImageToStream(image,out));
}
std::size_t idx(const juce::String& id){const auto r=parameterRegistry();for(std::size_t i=0;i<r.size();++i)if(r[i].id==id)return i;std::cout<<"no parameter "<<id<<'\n';++failures;return 0;}
bool reads(std::size_t i,float v,const juce::String& expected)
{
    const auto text=formatParameter(i,v);
    if(text!=expected)std::cout<<"  "<<parameterRegistry()[i].id<<" at "<<v<<" reads '"<<text<<"', not '"<<expected<<"'\n";
    return text==expected;
}
bool parses(std::size_t i,const juce::String& text,const juce::String& shown,std::optional<float> expected)
{
    const auto v=parseParameter(i,text,shown);
    const bool ok=v.has_value()==expected.has_value()&&(!v||std::abs(*v-*expected)<=1e-4f*std::max(1.0f,std::abs(*expected)));
    if(!ok)std::cout<<"  "<<parameterRegistry()[i].id<<" '"<<text<<"' shown in '"<<shown<<"' reads "<<(v?juce::String(*v):juce::String("nothing"))<<", not "<<(expected?juce::String(*expected):juce::String("nothing"))<<'\n';
    return ok;
}

// The prototype's shownValue: three figures (no decimals from 99.95, one from 9.995, else two); kHz from 999.5 Hz;
// ms below 0.9995 s and seconds from 999.5 ms; a sign on dB, st, cent, oct and a bipolar value without a unit;
// choices in capitals.
void values_format()
{
    section(__func__);
    const auto cutoff=idx("synth.1.analog.filter.cutoff"),attack=idx("synth.1.analog.amp.attack"),predelay=idx("effect.1.reverb.predelay_ms"),release=idx("effect.1.compressor.release_ms");
    CHECK(cutoff==39);
    CHECK(reads(cutoff,1799.999878f,"1.80 kHz"));CHECK(reads(cutoff,440.0f,"440 Hz"));CHECK(reads(cutoff,999.4f,"999 Hz"));
    CHECK(reads(cutoff,999.5f,"1.00 kHz"));CHECK(reads(cutoff,20000.0f,"20.0 kHz"));CHECK(reads(cutoff,20.0f,"20.0 Hz"));
    CHECK(reads(attack,0.25f,"250 ms"));CHECK(reads(attack,1.5f,"1.50 s"));CHECK(reads(attack,0.999f,"999 ms"));CHECK(reads(attack,1.0f,"1.00 s"));
    CHECK(reads(attack,0.005f,"5.00 ms"));CHECK(reads(attack,0.0f,"0.00 ms"));CHECK(reads(attack,20.0f,"20.0 s"));
    CHECK(reads(predelay,120.0f,"120 ms"));CHECK(reads(release,1500.0f,"1.50 s"));CHECK(reads(release,999.5f,"1.00 s"));CHECK(reads(release,999.0f,"999 ms"));
    CHECK(reads(idx("effect.1.compressor.threshold_db"),-18.0f,"-18.0 dB"));CHECK(reads(idx("effect.1.compressor.threshold_db"),0.0f,"0.00 dB"));
    CHECK(reads(idx("effect.1.compressor.makeup_db"),3.0f,"+3.00 dB"));CHECK(reads(idx("effect.1.drive.gain_db"),12.0f,"+12.0 dB"));
    CHECK(reads(idx("output.gain_db"),-6.0f,"-6.00 dB"));
    CHECK(reads(idx("synth.1.analog.osc.1.tune"),7.0f,"+7.00 st"));CHECK(reads(idx("synth.1.analog.osc.1.tune"),-12.0f,"-12.0 st"));
    CHECK(reads(idx("synth.1.analog.osc.1.detune"),-5.0f,"-5.00 cent"));
    CHECK(reads(idx("synth.1.analog.filter_env.octaves"),2.0f,"+2.00 oct"));
    CHECK(reads(idx("synth.1.lfo.1.beats"),0.25f,"0.25 beats"));CHECK(reads(idx("synth.1.lfo.1.beats"),16.0f,"16.0 beats"));
    CHECK(reads(idx("synth.1.analog.filter.mode"),1.0f,"BANDPASS"));CHECK(reads(idx("synth.1.modulation.1.source"),1.0f,"AMP ENVELOPE"));
    CHECK(reads(idx("output.protection"),1.0f,"ON"));CHECK(reads(idx("output.protection"),0.0f,"OFF"));
    CHECK(reads(idx("synth.1.analog.filter.resonance"),0.25f,"0.25"));CHECK(reads(idx("effect.1.compressor.ratio"),4.0f,"4.00"));CHECK(reads(idx("effect.1.compressor.ratio"),20.0f,"20.0"));
    for(const auto* tail:{"mix","full.mix","low.mix","mid.mix","high.mix"}){const auto i=idx("effect.7.saturation."+juce::String(tail));CHECK(reads(i,0,"0.00 %")&&reads(i,.5f,"50.0 %")&&reads(i,1,"100 %")&&unitShown(i,.5f)=="%");CHECK(parameterRegistry()[i].unit.isEmpty()&&parameterRegistry()[i].minimum==0&&parameterRegistry()[i].maximum==1);}
    const auto pan=idx("synth.1.pan");
    CHECK(reads(pan,-0.3f,"-0.30"));CHECK(reads(pan,0.3f,"+0.30"));CHECK(reads(pan,0.0f,"0.00"));CHECK(reads(pan,-0.0001f,"0.00"));
    CHECK(unitShown(cutoff,1799.999878f)=="kHz");CHECK(unitShown(cutoff,440.0f)=="Hz");CHECK(unitShown(attack,0.25f)=="ms");CHECK(unitShown(attack,1.5f)=="s");
    CHECK(unitShown(release,1500.0f)=="s");CHECK(unitShown(release,120.0f)=="ms");CHECK(unitShown(pan,0.3f).isEmpty());CHECK(unitShown(idx("synth.1.analog.filter.mode"),1.0f).isEmpty());
}

// A number, an optional prefix and an optional unit, in any case, or a choice's name; a bare number in the unit
// shown; out of range, not finite, another unit or not a value reads nothing.
void values_parse()
{
    section(__func__);
    const auto cutoff=idx("synth.1.analog.filter.cutoff"),attack=idx("synth.1.analog.amp.attack"),predelay=idx("effect.1.reverb.predelay_ms"),release=idx("effect.1.compressor.release_ms");
    CHECK(parses(cutoff,"1.80 kHz","kHz",1800.0f));CHECK(parses(cutoff,"440","Hz",440.0f));CHECK(parses(cutoff,"1.2","kHz",1200.0f));
    CHECK(parses(cutoff,"1.2k","Hz",1200.0f));CHECK(parses(cutoff,"1.2 KHZ","Hz",1200.0f));CHECK(parses(cutoff,"880hz","kHz",880.0f));
    CHECK(parses(cutoff," 440 Hz ","kHz",440.0f));CHECK(parses(cutoff,"20.04 kHz","kHz",20000.0f));CHECK(parses(cutoff,"19.97","Hz",20.0f));CHECK(parses(cutoff,"19.5","Hz",{}));
    CHECK(parses(cutoff,"99999","Hz",{}));CHECK(parses(cutoff,"3 dB","Hz",{}));CHECK(parses(cutoff,"abc","Hz",{}));CHECK(parses(cutoff,"","Hz",{}));
    CHECK(parses(cutoff,"inf","Hz",{}));CHECK(parses(cutoff,"nan","Hz",{}));CHECK(parses(cutoff,"1e999","Hz",{}));CHECK(parses(cutoff,"440 Hz Hz","Hz",{}));CHECK(parses(cutoff,"1.2 mHz","Hz",{}));
    CHECK(parses(attack,"250","ms",0.25f));CHECK(parses(attack,"0.25 s","ms",0.25f));CHECK(parses(attack,"250m","s",0.25f));CHECK(parses(attack,"250 MS","s",0.25f));
    CHECK(parses(attack,"1.5","s",1.5f));CHECK(parses(attack,"-1","s",{}));CHECK(parses(attack,"21 s","s",{}));
    CHECK(parses(predelay,"120","ms",120.0f));CHECK(parses(predelay,"0.12 s","ms",120.0f));CHECK(parses(release,"1.5","s",1500.0f));CHECK(parses(release,"2 s","ms",2000.0f));
    const auto threshold=idx("effect.1.compressor.threshold_db");
    CHECK(parses(threshold,"-12","dB",-12.0f));CHECK(parses(threshold,"-12 db","dB",-12.0f));CHECK(parses(threshold,"-12k","dB",{}));CHECK(parses(threshold,"-12 s","dB",{}));
    CHECK(parses(idx("synth.1.analog.osc.1.tune"),"+7 st","st",7.0f));CHECK(parses(idx("synth.1.analog.osc.1.tune"),"7","st",7.0f));
    CHECK(parses(idx("synth.1.analog.osc.1.detune"),"-5 CENT","cent",-5.0f));CHECK(parses(idx("synth.1.analog.filter_env.octaves"),"2 oct","oct",2.0f));
    CHECK(parses(idx("synth.1.lfo.1.beats"),"0.25 beats","beats",0.25f));CHECK(parses(idx("synth.1.lfo.1.beats"),"0.06","beats",0.0625f));
    const auto mode=idx("synth.1.analog.filter.mode");
    CHECK(parses(mode,"bandpass","",1.0f));CHECK(parses(mode,"BANDPASS","",1.0f));CHECK(parses(mode," Highpass ","",2.0f));CHECK(parses(mode,"1","",{}));CHECK(parses(mode,"notch","",{}));
    CHECK(parses(idx("synth.1.modulation.1.source"),"amp envelope","",1.0f));
    CHECK(parses(idx("output.protection"),"on","",1.0f));CHECK(parses(idx("output.protection"),"OFF","",0.0f));
    for(const auto* tail:{"mix","full.mix","low.mix","mid.mix","high.mix"}){const auto i=idx("effect.7.saturation."+juce::String(tail));CHECK(parses(i,"0%","%",0.f)&&parses(i,"50%","%",.5f)&&parses(i,"100%","%",1.f));CHECK(parses(i,"50","%",.5f)&&parses(i,"50%","",.5f)&&parses(i,"0.5","",.5f));CHECK(parses(i,"101%","%",{})&&parses(i,"-1%","%",{})&&parses(i,"nan%","%",{}));}
    const auto resonance=idx("synth.1.analog.filter.resonance");
    CHECK(parses(resonance,"0.5","",0.5f));CHECK(parses(resonance,"0.5 Hz","",{}));CHECK(parses(resonance,"2","",{}));CHECK(parses(resonance,"0.5k","",{}));
    CHECK(parses(idx("synth.1.pan"),"-0.30","",-0.3f));CHECK(parses(idx("synth.1.pan"),"+0.3","",0.3f));
}

// The display's resolution, in the registry's unit: half the last digit shown.
float resolution(std::size_t i,const juce::String& text,const juce::String& unit)
{
    const auto number=text.upToFirstOccurrenceOf(" ",false,false);
    const auto decimals=number.containsChar('.')?number.fromFirstOccurrenceOf(".",false,false).length():0;
    const auto& registryUnit=parameterRegistry()[i].unit;
    const float scale=unit=="kHz"?1000.0f:unit=="ms"&&registryUnit=="s"?0.001f:unit=="s"&&registryUnit=="ms"?1000.0f:unit=="%"&&registryUnit.isEmpty()?.01f:1.0f;
    return 0.5f*std::pow(10.0f,-(float)decimals)*scale;
}

// For every continuous parameter, parse(format(v)) is v within the display's resolution, at its minimum, default and
// maximum and at 32 points between, typed with its unit and as a bare number in the unit shown.
void values_round_trip()
{
    section(__func__);
    const auto r=parameterRegistry();int tried=0,wrong=0;
    for(std::size_t i=0;i<r.size();++i)
    {
        const auto& d=r[i];if(d.kind!=ParameterKind::continuous)continue;
        const juce::NormalisableRange<float> range(d.minimum,d.maximum,0.0f,d.skew);
        std::vector<float> values{d.minimum,d.defaultValue,d.maximum};
        for(int k=1;k<=32;++k)values.push_back(range.convertFrom0to1((float)k/33.0f));
        for(const auto v:values)
        {
            const auto text=formatParameter(i,v),unit=unitShown(i,v),bare=text.upToFirstOccurrenceOf(" ",false,false);
            const auto tolerance=resolution(i,text,unit)+1e-5f*std::max(1.0f,std::abs(v));
            for(const auto& typed:{text,bare})
            {
                ++tried;const auto p=parseParameter(i,typed,unit);
                if(!p||std::abs(*p-v)>tolerance){if(++wrong<=10)std::cout<<"  "<<d.id<<" at "<<v<<": '"<<typed<<"' reads "<<(p?juce::String(*p):juce::String("nothing"))<<'\n';}
            }
        }
    }
    std::cout<<"  "<<tried<<" entries, "<<wrong<<" wrong\n";
    CHECK(tried>0&&wrong==0);
}

juce::MouseEvent event(juce::Component& c,Pt at,juce::ModifierKeys mods,int clicks,bool dragged,std::optional<Pt> down={})
{
    const auto t=juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),at,mods,juce::MouseInputSource::defaultPressure,
        juce::MouseInputSource::defaultOrientation,juce::MouseInputSource::defaultRotation,juce::MouseInputSource::defaultTiltX,
        juce::MouseInputSource::defaultTiltY,&c,&c,t,down.value_or(at),t,clicks,dragged);
}
Pt local(juce::Component& c,Pt inParent){return inParent-c.getPosition().toFloat();}
// A press, ten moves and a release on the control's own mouse handlers, between points in its parent.
void drag(juce::Component& c,Pt from,Pt to,juce::ModifierKeys extra={})
{
    const auto a=local(c,from),b=local(c,to);
    const auto held=juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier|extra.getRawFlags());
    c.mouseDown(event(c,a,held,1,false));
    for(int i=1;i<=10;++i)c.mouseDrag(event(c,a+(b-a)*((float)i/10.0f),held,1,true,a));
    c.mouseUp(event(c,b,{},1,true,a));
}
juce::MouseWheelDetails wheel(float deltaY){juce::MouseWheelDetails w;w.deltaX=0.0f;w.deltaY=deltaY;w.isReversed=w.isSmooth=w.isInertial=false;return w;}
double luma(juce::Colour c){return 255.0*(0.114*c.getFloatBlue()+0.587*c.getFloatGreen()+0.299*c.getFloatRed());}
double meanLuma(const juce::Image& img,Rect r)
{
    const auto b=r.getSmallestIntegerContainer().getIntersection(img.getBounds());
    const juce::Image::BitmapData bd(img,juce::Image::BitmapData::readOnly);
    double sum=0;
    for(int y=b.getY();y<b.getBottom();++y)for(int x=b.getX();x<b.getRight();++x)sum+=luma(bd.getPixelColour(x,y));
    return sum/std::max(1,b.getWidth()*b.getHeight());
}
SoundDocument document(InstrumentProcessor& p){SoundDocument d;std::uint64_t r=0;CHECK(p.trySoundSnapshot(d,r));return d;}
bool edit(InstrumentProcessor& p,const SoundDocument& d){SoundDocument now;std::uint64_t r=0;CHECK(p.trySoundSnapshot(now,r));return p.applySoundEdit(d,SoundEditOrigin::manual,r);}
juce::String menuText(ProgrammerKnob& k){juce::StringArray s;for(auto& item:k.menu())s.add(item.first);return s.joinIntoString("|");}
bool perform(ProgrammerKnob& k,const juce::String& name)
{
    for(auto& item:k.menu())if(item.first==name){item.second();return true;}
    std::cout<<"  no menu item "<<name<<'\n';return false;
}

// Knobs over a real processor in a parent with the PROGRAMMER's look, on the desktop, as the unit holds them. The host
// records what it is told to say, and its change is the editor's: the edit on a snapshot with today's sentences, then
// the refresh. A listener on each knob's parameter counts what a host sees: gesture begins and ends, and value changes.
struct KnobRig
{
    struct Counts{int begins=0,ends=0,changes=0;};
    struct Counter final:juce::AudioProcessorParameter::Listener
    {
        Counts counts;
        void parameterValueChanged(int,float) override{++counts.changes;}
        void parameterGestureChanged(int,bool starting) override{++(starting?counts.begins:counts.ends);}
    };
    static companion::ClientOptions offline(){companion::ClientOptions o;o.allowLaunch=false;return o;}
    InstrumentProcessor p{offline()};
    const Model& model=*studioSkin();
    ProgrammerLook look{model};
    house::Host host;
    juce::StringArray reported;
    juce::Component holder;
    std::vector<std::unique_ptr<ProgrammerKnob>> knobs;
    std::map<std::size_t,std::unique_ptr<Counter>> counters;
    ProgrammerKnob* touched=nullptr;
    KnobRig(std::initializer_list<std::size_t> indices)
    {
        host.frame=[]{return std::shared_ptr<void>();};
        host.closing=[]{return false;};
        host.report=[this](const juce::String& s){reported.add(s);};
        host.change=[this](const std::function<void(SoundDocument&)>& change,const juce::String& caption)
        {
            SoundDocument d;std::uint64_t at=0;
            if(p.trySoundSnapshot(d,at)){change(d);reported.add(caption+(p.applySoundEdit(d,SoundEditOrigin::manual,at)?" complete":" failed; sound preserved"));}
            else reported.add("Sound is busy; retry the edit");
            refresh();
        };
        holder.setLookAndFeel(&look);holder.setSize(1547,754);
        float x=160.0f;
        for(const auto i:indices)
        {
            auto k=std::make_unique<ProgrammerKnob>(p,look,host,i);
            k->place({x,338.0f});x+=180.0f;
            k->onTouched=[this](ProgrammerKnob& t){touched=&t;};
            holder.addAndMakeVisible(*k);holder.addAndMakeVisible(k->label);holder.addChildComponent(k->entry);
            auto c=std::make_unique<Counter>();p.control(i).addListener(c.get());counters[i]=std::move(c);
            knobs.push_back(std::move(k));
        }
        holder.addToDesktop(0);holder.setVisible(true);refresh();
    }
    ~KnobRig(){for(auto& [i,c]:counters)p.control(i).removeListener(c.get());knobs.clear();holder.setLookAndFeel(nullptr);}
    ProgrammerKnob& knob(std::size_t i){for(auto& k:knobs)if(k->index==i)return *k;throw std::runtime_error("no knob");}
    void refresh(){const auto d=document(p);for(auto& k:knobs)k->setLocked(d.controlLocks[k->index]);}
    Counts counts(std::size_t i) const{return counters.at(i)->counts;}
    template<class F> Counts delta(std::size_t i,F act){const auto b=counts(i);act();const auto a=counts(i);return {a.begins-b.begins,a.ends-b.ends,a.changes-b.changes};}
    float normalised(std::size_t i){return p.control(i).getValue();}
    double physical(std::size_t i){auto& c=p.control(i);return c.convertFrom0to1(c.getValue());}
    // What a plug-in wrapper does for host automation: the value and its listeners, no gesture.
    void automate(std::size_t i,double v){auto& c=p.control(i);c.setValueNotifyingHost(c.convertTo0to1((float)v));}
    void lock(std::size_t i,bool on){auto d=document(p);d.controlLocks[i]=on;CHECK(edit(p,d));refresh();}
    double lumaAt(Rect r){return meanLuma(holder.createComponentSnapshot(holder.getLocalBounds(),true,1.0f),r);}
};

// The prototype's check 41 against the processor: CUTOFF's pointer follows its value over the sweep; a 50 px drag turns
// RESO a quarter and 101 px moves the 3-way MODE one detent, each then the last touched control; a double click opens
// the entry in the display's form, 0.44 typed in its kHz sets CUTOFF 440 Hz and 99999 changes nothing; the menu
// unlocks, locks (the dot beside the label) and resets.
void programmer_knobs()
{
    section(__func__);
    KnobRig rig{39,40,41};
    auto& cutoff=rig.knob(39);auto& reso=rig.knob(40);auto& mode=rig.knob(41);
    CHECK(cutoff.getComponentID()=="synth.1.analog.filter.cutoff"&&cutoff.label.getText()=="CUTOFF"&&cutoff.steps()==0&&mode.steps()==2);
    rig.automate(39,1799.999878);
    CHECK(std::abs(cutoff.angle()-(-144.5f+289.0f*rig.normalised(39)))<0.01f);
    const auto r0=rig.normalised(40);
    drag(reso,reso.centre(),reso.centre()-Pt(0.0f,50.0f));
    CHECK(std::abs(rig.normalised(40)-std::min(1.0f,r0+0.25f))<0.002f);
    CHECK(rig.touched==&reso&&reso.valueLine().startsWith("RESO "));
    rig.automate(41,0.0);
    drag(mode,mode.centre(),mode.centre()-Pt(0.0f,101.0f));
    CHECK(rig.physical(41)==1.0&&mode.valueLine()=="MODE BANDPASS"&&rig.touched==&mode);
    const auto doubleClick=[&]{cutoff.mouseDoubleClick(event(cutoff,local(cutoff,cutoff.centre()),juce::ModifierKeys::leftButtonModifier,2,false));};
    CHECK(!cutoff.entry.isCaretVisible());
    doubleClick();
    CHECK(cutoff.entry.isVisible()&&cutoff.entry.getText()=="1.80 kHz");
    CHECK(cutoff.entry.isCaretVisible());
    cutoff.entry.setText("0.44");cutoff.commitEntry();
    CHECK(std::abs(rig.physical(39)-440.0)<0.05&&cutoff.valueLine()=="CUTOFF 440 Hz"&&!cutoff.entry.isVisible()&&rig.touched==&cutoff);
    CHECK(!cutoff.entry.isCaretVisible());
    doubleClick();cutoff.entry.setText("99999");cutoff.commitEntry();
    CHECK(std::abs(rig.physical(39)-440.0)<0.05&&!cutoff.entry.isVisible());
    cutoff.openEntry();CHECK(cutoff.entry.isCaretVisible());cutoff.closeEntry();
    CHECK(!cutoff.entry.isVisible()&&!cutoff.entry.isCaretVisible());
    rig.lock(39,true);
    const auto lockedItems=menuText(cutoff);const auto lockedLuma=rig.lumaAt(cutoff.lockArea());
    CHECK(lockedItems=="Unlock|Default"&&cutoff.isLocked());
    CHECK(perform(cutoff,"Unlock")&&!document(rig.p).controlLocks[39]&&rig.reported.joinIntoString("|")=="Control lock complete"&&!cutoff.isLocked());
    const auto unlockedLuma=rig.lumaAt(cutoff.lockArea());
    CHECK(menuText(cutoff)=="Lock|Default");
    CHECK(perform(cutoff,"Lock")&&document(rig.p).controlLocks[39]&&cutoff.isLocked());
    CHECK(std::abs(rig.normalised(39)-rig.p.control(39).getDefaultValue())>0.01f);
    CHECK(perform(cutoff,"Default")&&std::abs(rig.normalised(39)-rig.p.control(39).getDefaultValue())<1e-6f);
    std::cout<<"  menu "<<lockedItems<<"; dot luma "<<lockedLuma<<" locked, "<<unlockedLuma<<" unlocked\n";
    CHECK(std::abs(lockedLuma-unlockedLuma)>15.0);
}

// The house unit's gesture rules: a drag is one gesture, a stepped drag editing only as its detent changes and Shift
// dragging five times finer; each key or wheel step, entry or Default is one complete gesture; an edit the processor
// refuses re-reads the value and says today's sentence; Escape during a drag returns the start value and ends its
// gesture.
void programmer_gestures()
{
    section(__func__);
    KnobRig rig{39,40,41};
    auto& cutoff=rig.knob(39);auto& reso=rig.knob(40);auto& mode=rig.knob(41);
    auto d=rig.delta(39,[&]{drag(cutoff,cutoff.centre(),cutoff.centre()-Pt(0.0f,40.0f));});
    CHECK(d.begins==1&&d.ends==1&&d.changes>=2);
    rig.automate(41,0.0);
    d=rig.delta(41,[&]{drag(mode,mode.centre(),mode.centre()-Pt(0.0f,101.0f));});
    CHECK(d.begins==1&&d.ends==1&&d.changes==1);
    const auto r0=rig.normalised(40);
    drag(reso,reso.centre(),reso.centre()-Pt(0.0f,100.0f),juce::ModifierKeys::shiftModifier);
    CHECK(std::abs(rig.normalised(40)-(r0+0.1f))<0.002f);
    const auto complete=[&](const char* what,std::size_t i,auto act,float expected)
    {
        const auto c=rig.delta(i,act);const bool ok=c.begins==1&&c.ends==1&&std::abs(rig.normalised(i)-expected)<1e-4f;
        if(!ok)std::cout<<"  "<<what<<": "<<c.begins<<" begins, "<<c.ends<<" ends, at "<<rig.normalised(i)<<", not "<<expected<<'\n';
        return ok;
    };
    rig.automate(39,440.0);const auto n=rig.normalised(39);
    CHECK(complete("up",39,[&]{cutoff.keyPressed(juce::KeyPress(juce::KeyPress::upKey));},n+0.01f));
    CHECK(complete("page down",39,[&]{cutoff.keyPressed(juce::KeyPress(juce::KeyPress::pageDownKey));},n-0.09f));
    {const auto held=rig.normalised(39);studio::noteRackScroll();cutoff.mouseWheelMove(event(cutoff,local(cutoff,cutoff.centre()),{},0,false),wheel(0.5f));CHECK(rig.normalised(39)==held);juce::Thread::sleep(450);}
    CHECK(complete("wheel",39,[&]{cutoff.mouseWheelMove(event(cutoff,local(cutoff,cutoff.centre()),{},0,false),wheel(0.5f));},n-0.08f));
    CHECK(complete("end",39,[&]{cutoff.keyPressed(juce::KeyPress(juce::KeyPress::endKey));},1.0f));
    CHECK(complete("home",39,[&]{cutoff.keyPressed(juce::KeyPress(juce::KeyPress::homeKey));},0.0f));
    CHECK(complete("entry",39,[&]{cutoff.openEntry();cutoff.entry.setText("2 kHz");cutoff.commitEntry();},rig.p.control(39).convertTo0to1(2000.0f)));
    CHECK(complete("default",39,[&]{perform(cutoff,"Default");},rig.p.control(39).getDefaultValue()));
    CHECK(complete("mode down",41,[&]{mode.keyPressed(juce::KeyPress(juce::KeyPress::downKey));},0.0f));
    CHECK(rig.p.beginParameterGesture(40));
    const auto held=rig.normalised(39);rig.reported.clear();
    cutoff.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(rig.normalised(39)==held&&std::abs(cutoff.getValue()-rig.physical(39))<1e-3*rig.physical(39)&&rig.reported.joinIntoString("|")==rejectedEdit);
    rig.reported.clear();
    d=rig.delta(39,[&]{drag(cutoff,cutoff.centre(),cutoff.centre()-Pt(0.0f,40.0f));});
    CHECK(rig.normalised(39)==held&&!cutoff.isDragging()&&d.begins==0&&rig.reported.joinIntoString("|")==rejectedEdit);
    CHECK(rig.p.endParameterGesture(40));
    const auto start=rig.normalised(39);bool moved=false,escaped=false;
    d=rig.delta(39,[&]
    {
        const auto at=local(cutoff,cutoff.centre());const juce::ModifierKeys button(juce::ModifierKeys::leftButtonModifier);
        cutoff.mouseDown(event(cutoff,at,button,1,false));
        cutoff.mouseDrag(event(cutoff,at-Pt(0.0f,30.0f),button,1,true,at));
        moved=rig.normalised(39)!=start;
        escaped=cutoff.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        cutoff.mouseDrag(event(cutoff,at-Pt(0.0f,60.0f),button,1,true,at));
        cutoff.mouseUp(event(cutoff,at-Pt(0.0f,60.0f),{},1,true,at));
    });
    CHECK(moved&&escaped&&std::abs(rig.normalised(39)-start)<1e-6f&&d.begins==1&&d.ends==1&&!cutoff.isDragging());
    CHECK(rig.p.beginParameterGesture(39)&&rig.p.endParameterGesture(39));
}

// The knob's accessible role: a slider titled by the registry's name whose value is its display text; a value set as
// text (with or without its name) or as a number moves the parameter in one gesture, and text that is not a value
// leaves it.
void programmer_roles()
{
    section(__func__);
    KnobRig rig{39,41};
    auto& cutoff=rig.knob(39);
    rig.automate(39,1799.999878);
    auto* h=cutoff.getAccessibilityHandler();
    CHECK(h!=nullptr&&h->getRole()==juce::AccessibilityRole::slider&&h->getTitle()==parameterRegistry()[39].name);
    auto* v=h!=nullptr?h->getValueInterface():nullptr;
    CHECK(v!=nullptr&&v->getCurrentValueAsString()=="CUTOFF 1.80 kHz");
    if(v!=nullptr)
    {
        const auto d=rig.delta(39,[&]{v->setValueAsString("440 Hz");});
        CHECK(std::abs(rig.physical(39)-440.0)<0.05&&d.begins==1&&d.ends==1);
        v->setValueAsString("CUTOFF 2.5 kHz");
        CHECK(std::abs(rig.physical(39)-2500.0)<0.5);
        v->setValueAsString("loud");
        CHECK(std::abs(rig.physical(39)-2500.0)<0.5);
        v->setValue(1000.0);
        CHECK(std::abs(rig.physical(39)-1000.0)<0.05);
    }
    rig.automate(41,2.0);
    auto* mh=rig.knob(41).getAccessibilityHandler();
    auto* mv=mh!=nullptr?mh->getValueInterface():nullptr;
    CHECK(mv!=nullptr&&mv->getCurrentValueAsString()=="MODE HIGHPASS");
    if(mv!=nullptr){mv->setValueAsString("bandpass");CHECK(rig.physical(41)==1.0);}
}

// An example rack placed with an explicit allocation (local unit, zero-based physical bank).
juce::Uuid placeRack(InstrumentProcessor& p,std::size_t example,RackDestination destination,std::vector<UnitBinding> allocation,bool reuse=false)
{
    RackOperation op;op.design=airack::test::frozenExampleRackDesigns()[example];op.newInstance=juce::Uuid();op.destination=destination;
    op.destination.role=op.design->role;op.allocation=std::move(allocation);op.allowSlotReuse=reuse;
    SoundDocument d;std::uint64_t revision=0;
    CHECK(p.trySoundSnapshot(d,revision)&&p.applyRackOperation(op,revision)==RackCandidateStatus::ready);
    return op.newInstance;
}
// The house prototype's drawn state, as StudioWindowTests places it: Ember Reed in synth bank 1 with its drive in effect
// bank 1, Glass Current in bank 2 with its delay and reverb in effect banks 2 and 3, Thread Press on the master chain in
// effect banks 4 and 5; then macros 1 to 4 on AIR +40, FACET -25, ECHO +60 and GRIP +30.
void drawnRacks(InstrumentProcessor& p)
{
    placeRack(p,0,{},{{0,0},{1,0}},true);
    placeRack(p,1,{RackRole::synth,masterChain,1},{{0,1},{1,1},{2,2}});
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
void click(juce::Component& c,Pt at){c.mouseDown(event(c,at,juce::ModifierKeys::leftButtonModifier,1,false));c.mouseUp(event(c,at,{},1,false));}
void click(SkinButton& b){click(b,b.element.rect.getCentre()-b.getPosition().toFloat());}
void click(juce::Button& b){click(b,b.getLocalBounds().toFloat().getCentre());}
juce::Component* byId(juce::Component& root,const juce::String& id)
{
    if(root.getComponentID()==id)return &root;
    for(auto* c:root.getChildren())if(auto* found=byId(*c,id))return found;
    return nullptr;
}
template<class T> T* widget(juce::Component& root,const juce::String& id){return dynamic_cast<T*>(byId(root,id));}

// The editor's host as the rigs give it. It records what it is told to say; its change is the editor's (the edit on a
// snapshot with today's sentences, then the refresh), its snapshot says the editor's busy sentence when it fails, and
// its refresh shows the rig's component the document.
ProgrammerHost rigHost(InstrumentProcessor& p,juce::StringArray& reported,std::function<void()> refresh)
{
    ProgrammerHost h;
    h.house.frame=[]{return std::shared_ptr<void>();};
    h.house.closing=[]{return false;};
    h.house.report=[&reported](const juce::String& s){reported.add(s);};
    h.house.change=[&p,&reported,refresh](const std::function<void(SoundDocument&)>& change,const juce::String& caption)
    {
        SoundDocument d;std::uint64_t at=0;
        if(p.trySoundSnapshot(d,at)){change(d);reported.add(caption+(p.applySoundEdit(d,SoundEditOrigin::manual,at)?" complete":" failed; sound preserved"));}
        else reported.add("Sound is busy; retry the edit");
        refresh();
    };
    h.snapshot=[&p,&reported](SoundDocument& d,std::uint64_t& at){if(p.trySoundSnapshot(d,at))return true;reported.add("Sound is busy; retry the edit");return false;};
    h.refresh=refresh;
    return h;
}

// The PROGRAMMER over a real processor and the embedded skin, alone on the desktop at 1x (decision 14), with the rigs'
// host.
struct UnitRig
{
    static companion::ClientOptions offline(){companion::ClientOptions o;o.allowLaunch=false;return o;}
    InstrumentProcessor p{offline()};
    const Model& model=*studioSkin();
    juce::StringArray reported;
    juce::Component holder;
    std::unique_ptr<ProgrammerUnit> unit;
    UnitRig()
    {
        CHECK(blankInstance(p));
        unit=std::make_unique<ProgrammerUnit>(p,model,rigHost(p,reported,[this]{refresh();}));
        holder.setSize(unit->getWidth(),unit->getHeight());holder.addAndMakeVisible(*unit);
        holder.addToDesktop(0);holder.setVisible(true);refresh();
    }
    ~UnitRig(){unit.reset();}
    void refresh(){if(unit)unit->refresh(document(p));}
    ProgrammerKnob* knob(std::size_t i){return unit->knob(i);}
    double physical(std::size_t i){auto& c=p.control(i);return c.convertFrom0to1(c.getValue());}
    // A press and a release at the knob's middle: it becomes the last touched control, its value unchanged.
    void touch(std::size_t i)
    {
        auto* k=knob(i);CHECK(k!=nullptr);if(k==nullptr)return;
        const Pt c((float)k->getWidth()*0.5f,(float)k->getHeight()*0.5f);
        k->mouseDown(event(*k,c,juce::ModifierKeys::leftButtonModifier,1,false));k->mouseUp(event(*k,c,{},1,false));
    }
    // The drawn state: the drawn racks and macros, CUTOFF at 1.80 kHz as drawn (placing Ember Reed on the bank it reuses
    // keeps the bank's values), SYNTH 1's ANALOG shown and CUTOFF last touched.
    void drawn()
    {
        drawnRacks(p);drawnMacros(p);
        auto& cutoff=p.control(39);cutoff.setValueNotifyingHost(cutoff.convertTo0to1(1800.0f));
        refresh();unit->showPage("synth1");unit->showSubpage("analog");touch(39);
    }
    // A present effect of the kind in the bank, last on the chain, through the processor's rack edit.
    void addEffect(std::uint8_t slot,std::uint8_t chain,EffectKind kind)
    {
        SoundDocument d;std::uint64_t at=0;
        CHECK(p.trySoundSnapshot(d,at));
        CHECK(p.applyRackEdit(RackEdit{RackEditKind::addEffect,slot,chain,d.layout.effectCount[chain],kind},at)==RackEditResult::applied);
        refresh();
    }
    juce::String litPages()
    { juce::StringArray s;for(auto& b:unit->pageButtons)if(b->lit())s.add(b->element.label);return s.joinIntoString(","); }
    juce::String subpages()
    {
        juce::StringArray s;
        for(auto& b:unit->subpageButtons)if(b->isVisible())s.add(b->getButtonText()+(b->getToggleState()?"*":""));
        return s.joinIntoString(",");
    }
    juce::Button* subpageButton(const juce::String& text)
    { for(auto& b:unit->subpageButtons)if(b->isVisible()&&b->getButtonText()==text)return b.get();return nullptr; }
    juce::String said() const { return reported.isEmpty()?juce::String():reported[reported.size()-1]; }
    juce::Image snap(){return holder.createComponentSnapshot(holder.getLocalBounds(),true,1.0f);}
};

// Hidden numeric editors own no blinking caret; only an open entry does.
void programmer_numeric_caret_lifetime()
{
    section(__func__);
    auto owned=std::make_unique<UnitRig>();auto& rig=*owned;auto& u=*rig.unit;
    const auto caretCount=[](const juce::Component& root){
        const auto visit=[](const auto& self,const juce::Component& component)->int{
            int count=dynamic_cast<const juce::CaretComponent*>(&component)!=nullptr;
            for(auto* child:component.getChildren())count+=self(self,*child);
            return count;
        };return visit(visit,root);
    };
    std::size_t hidden=0,enabled=0,carets=0;
    for(std::size_t i=0;i<parameterCount;++i)if(auto* k=rig.knob(i);k&&!k->entry.isVisible()){
        ++hidden;enabled+=k->entry.isCaretVisible();carets+=caretCount(k->entry);
    }
    CHECK(hidden==parameterCount);CHECK(enabled==0);CHECK(carets==0);
    auto* cutoff=rig.knob(39);CHECK(cutoff!=nullptr);if(!cutoff)return;
    u.showPage("synth1");u.showSubpage("analog");
    const auto before=rig.p.control(39).getValue();cutoff->openEntry();
    CHECK(cutoff->entry.isVisible()&&cutoff->entry.isCaretVisible()&&caretCount(cutoff->entry)==1);
    CHECK(cutoff->entry.getHighlightedRegion().getLength()==cutoff->entry.getText().length());
    cutoff->entry.setText("440 Hz");cutoff->commitEntry();
    CHECK(!cutoff->entry.isVisible()&&!cutoff->entry.isCaretVisible()&&caretCount(cutoff->entry)==0);
    CHECK(std::abs(rig.physical(39)-440)<.05);CHECK(rig.p.undoSoundEdit());CHECK(rig.p.control(39).getValue()==before);
    cutoff->openEntry();cutoff->entry.setText("880 Hz");cutoff->entry.onEscapeKey();
    CHECK(!cutoff->entry.isVisible()&&!cutoff->entry.isCaretVisible()&&caretCount(cutoff->entry)==0);
    CHECK(rig.p.control(39).getValue()==before);
    cutoff->openEntry();u.showPage("global");juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    CHECK(!cutoff->entry.isVisible()&&!cutoff->entry.isCaretVisible()&&caretCount(cutoff->entry)==0);
    u.showPage("synth1");u.showSubpage("analog");cutoff->openEntry();
    CHECK(cutoff->entry.isVisible()&&cutoff->entry.isCaretVisible());
    u.setVisible(false);
    CHECK(!cutoff->entry.isVisible()&&!cutoff->entry.isCaretVisible()&&caretCount(cutoff->entry)==0);
    u.setVisible(true);
    CHECK(!cutoff->entry.isVisible()&&!cutoff->entry.isCaretVisible()&&caretCount(cutoff->entry)==0);
    const auto shelfIndex=idx("effect.1.space_reverb.post.1.shelf_q"),shapeIndex=idx("effect.1.space_reverb.post.1.shape");
    auto* shelf=rig.knob(shelfIndex);CHECK(shelf!=nullptr);if(!shelf)return;
    auto& shape=rig.p.control(shapeIndex);shape.setValueNotifyingHost(shape.convertTo0to1(1));rig.refresh();
    u.showPage("effects");u.showSubpage("fx1");
    for(std::size_t n=0;n<rig.model.page("effects").subpages.front().routes.size();++n){const auto shown=u.shownKnobs();if(std::find(shown.begin(),shown.end(),shelf)!=shown.end())break;u.showSubpage("fx1");}
    CHECK(shelf->isShowing()&&shelf->isEnabled());shelf->openEntry();
    CHECK(shelf->entry.isVisible()&&shelf->entry.isCaretVisible()&&caretCount(shelf->entry)==1);
    shape.setValueNotifyingHost(shape.convertTo0to1(0));rig.refresh();
    CHECK(!shelf->isEnabled()&&!shelf->entry.isVisible()&&!shelf->entry.isCaretVisible()&&caretCount(shelf->entry)==0);
}

// An all-controls panel over a real processor and the embedded skin, alone on the desktop at the stack's 1485 px, with
// the rigs' host, showing the units it is given.
struct PanelRig
{
    static companion::ClientOptions offline(){companion::ClientOptions o;o.allowLaunch=false;return o;}
    InstrumentProcessor p{offline()};
    const Model& model=*studioSkin();
    juce::StringArray reported;
    juce::Component holder;
    std::unique_ptr<AllControlsPanel> panel;
    explicit PanelRig(std::vector<AllControlsPanel::Unit> units)
    {
        CHECK(blankInstance(p));
        panel=std::make_unique<AllControlsPanel>(p,model,rigHost(p,reported,[this]{refresh();}));
        panel->setSize(1485,AllControlsPanel::panelHeight);
        holder.setSize(panel->getWidth(),panel->getHeight());holder.addAndMakeVisible(*panel);
        holder.addToDesktop(0);holder.setVisible(true);refresh();panel->show(std::move(units));
    }
    ~PanelRig(){panel.reset();}
    void refresh(){if(panel)panel->refresh(document(p));}
    ProgrammerKnob* knob(std::size_t i){return panel->knob(i);}
    double physical(std::size_t i){auto& c=p.control(i);return c.convertFrom0to1(c.getValue());}
    void touch(std::size_t i)
    {
        auto* k=knob(i);CHECK(k!=nullptr);if(k==nullptr)return;
        const Pt c((float)k->getWidth()*0.5f,(float)k->getHeight()*0.5f);
        k->mouseDown(event(*k,c,juce::ModifierKeys::leftButtonModifier,1,false));k->mouseUp(event(*k,c,{},1,false));
    }
    std::vector<std::size_t> shown() const{std::vector<std::size_t> v;for(auto* k:panel->shownKnobs())v.push_back(k->index);return v;}
    juce::Button* key(int n){return widget<juce::Button>(*panel,"rack.controls.key."+juce::String(n));}
    juce::String said() const { return reported.isEmpty()?juce::String():reported[reported.size()-1]; }
    juce::Image snap(){return holder.createComponentSnapshot(holder.getLocalBounds(),true,1.0f);}
};

void panel_numeric_caret_lifetime()
{
    section(__func__);
    auto owned=std::make_unique<PanelRig>(std::vector<AllControlsPanel::Unit>{{NativeModule::analog,0}});auto& rig=*owned;auto& panel=*rig.panel;
    auto* cutoff=rig.knob(39);CHECK(cutoff!=nullptr);if(!cutoff)return;
    CHECK(panel.showParameter(39));cutoff->openEntry();
    CHECK(cutoff->entry.isVisible()&&cutoff->entry.isCaretVisible());
    CHECK(panel.showParameter(12));
    CHECK(!cutoff->entry.isVisible()&&!cutoff->entry.isCaretVisible());
    CHECK(panel.showParameter(39));cutoff->openEntry();panel.setVisible(false);
    CHECK(!cutoff->entry.isVisible()&&!cutoff->entry.isCaretVisible());
    panel.setVisible(true);
    CHECK(!cutoff->entry.isVisible()&&!cutoff->entry.isCaretVisible());
}

// The prototype's check 40: the page buttons a radio group lit from the skin; the drawn state lights SYNTH 1 and its
// sub-pages, ANALOG shown; GLOBAL alone lit with 15 knobs and no sub-pages; EFFECTS 8 sub-pages, FX 1 shown with 23
// knobs; RACK LAYOUT 12 rows and no knobs; SYNTH 2's ROUTES from parameter 332, again 352, the fifth press back to 332.
void programmer_pages()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    CHECK(u.pageButtons.size()==7&&u.subpageButtons.size()==8);
    if(u.pageButtons.size()!=7)return;
    juce::StringArray ids;
    for(auto& b:u.pageButtons)ids.add(b->getComponentID());
    CHECK(ids.joinIntoString(",")=="programmer.page.global,programmer.page.synth1,programmer.page.synth2,programmer.page.synth3,"
                                  "programmer.page.synth4,programmer.page.effects,programmer.page.layout");
    rig.drawn();
    const auto drawnLit=rig.litPages(),drawnSubs=rig.subpages();
    std::cout<<"  drawn: page "<<drawnLit<<", sub-pages "<<drawnSubs<<'\n';
    CHECK(drawnLit=="SYNTH 1"&&drawnSubs=="COMMON,ANALOG*,WAVETABLE,WARMTH,LFO 1/2,ROUTES,DRUM");
    if(auto* analog=rig.subpageButton("ANALOG"))CHECK(analog->getComponentID()=="programmer.subpage.analog");
    const auto lens=[&](const juce::Image& shot,std::size_t i){const auto& e=u.pageButtons[i]->element;return meanLuma(shot,Rect(2*e.lensR,2*e.lensR).withCentre(e.led));};
    const auto shot=rig.snap();
    keep("programmer-synth1-analog",shot);
    const auto on=lens(shot,1),off=lens(shot,0);
    std::cout<<"  SYNTH 1's lens "<<on<<" lit, GLOBAL's "<<off<<'\n';
    CHECK(on>off+30.0);
    for(std::size_t i=0;i<u.pageButtons.size();++i)
    {
        click(*u.pageButtons[i]);
        int lit=0;
        for(auto& b:u.pageButtons)lit+=b->lit()?1:0;
        CHECK(lit==1&&u.pageButtons[i]->lit()&&u.pageButtons[i]->getToggleState()&&u.page()==rig.model.pages[i].id);
    }
    click(*u.pageButtons[0]);
    keep("programmer-global",rig.snap());
    CHECK(rig.litPages()=="GLOBAL"&&rig.subpages().isEmpty()&&u.shownKnobs().size()==15);
    CHECK(u.knob(0)!=nullptr&&u.knob(0)->isShowing()&&u.knob(39)!=nullptr&&!u.knob(39)->isShowing());
    click(*u.pageButtons[5]);
    keep("programmer-effects-fx1",rig.snap());
    const auto effects=rig.subpages();
    CHECK(effects.startsWith("FX 1*,FX 2,")&&juce::StringArray::fromTokens(effects,",","").size()==8&&u.shownKnobs().size()==23);
    click(*u.pageButtons[6]);
    keep("programmer-layout",rig.snap());
    const auto rows=u.rackRows();
    CHECK(u.shownKnobs().empty()&&rows.size()==12&&rig.subpages().isEmpty());
    CHECK(std::all_of(rows.begin(),rows.end(),[](juce::Button* r){return r!=nullptr&&r->isShowing();}));
    click(*u.pageButtons[2]);
    const auto firstOf=[&]{const auto k=u.shownKnobs();return k.size()==20?(int)k.front()->index:-1;};
    const auto routes=[&]{if(auto* b=rig.subpageButton("ROUTES"))click(*b);return firstOf();};
    const auto r1=routes();
    keep("programmer-synth2-routes",rig.snap());
    const auto r2=routes();
    routes();routes();
    const auto r5=routes();
    std::cout<<"  SYNTH 2's ROUTES from "<<r1<<", again "<<r2<<", the fifth press "<<r5<<'\n';
    CHECK(r1==332&&r2==352&&r5==332);
}

// The product's: over the 530 pages, each of the 4519 parameters has exactly one knob, carrying its parameter's ID and
// shown on exactly one page; a page shows its own knobs and no others.
void programmer_every_parameter()
{
    section(__func__);
    UnitRig rig;
    rig.addEffect(0,masterChain,EffectKind::eraReverb);
    auto& u=*rig.unit;
    std::vector<int> shownOn(parameterCount,0);
    int pages=0,strays=0,legacyPages=0,saturationPages=0,eraImages=0;
    for(const auto& page:rig.model.pages)
    {
        u.showPage(page.id);
        for(const auto& sub:page.subpages)
            for(int r=0;r<std::max(1,(int)sub.routes.size());++r)
            {
                u.showSubpage(sub.id);
                ++pages;
                const auto shown=u.shownKnobs();
                (std::any_of(shown.begin(),shown.end(),[](const auto* k){return k->index>=saturationParameterStart;})?saturationPages:legacyPages)++;
                if(page.id=="synth1"&&sub.id=="drum"&&(r==0||r==1||r==16))
                    keep("programmer-drum-"+juce::String(r),rig.snap());
                if(page.id=="synth1"&&sub.id=="wavetable.warmth"&&r==0)
                    keep("programmer-warmth",rig.snap());
                if(page.id=="effects"&&sub.effect==0&&!shown.empty()&&shown.front()->index==eraReverbParameterStart+7)
                {
                    CHECK(shown.size()==7&&shown.back()->index==eraReverbParameterStart+13);
                    CHECK(u.display!=nullptr&&u.display->programmer.bank.contains("ERA REVERB"));
                    ++eraImages;
                    keep("programmer-era",rig.snap());
                }
                for(auto* k:shown)++shownOn[k->index];
                std::size_t showing=0;
                for(std::size_t i=0;i<parameterCount;++i)if(auto* k=u.knob(i);k!=nullptr&&k->isShowing())++showing;
                if(showing!=shown.size()||std::any_of(shown.begin(),shown.end(),[](ProgrammerKnob* k){return !k->isShowing();}))++strays;
            }
    }
    int once=0,ids=0;
    for(std::size_t i=0;i<parameterCount;++i)
    {
        once+=shownOn[i]==1?1:0;
        if(auto* k=u.knob(i);k!=nullptr&&k->index==i&&k->getComponentID()==parameterRegistry()[i].id)++ids;
    }
    const std::pair<std::size_t,const char*> labels[]{{758,"QUALITY"},{804,"LOW FREQ"},{1077,"OFFSET"},{1111,"XOVER"},{1230,"MIX"}};
    for(const auto& [i,label]:labels)CHECK(rig.model.knobLabels[i]==label);
    std::cout<<"  "<<pages<<" pages, "<<once<<" of "<<parameterCount<<" parameters shown on exactly one, "<<ids<<" knobs with their IDs\n";
    const juce::StringArray saturationPageIds{"saturation.global","saturation.full","saturation.low","saturation.mid","saturation.high"};
    const auto banks=document(rig.p).layout.effects.size();
    int declaredBanks=0;std::set<int> bankIds;
    for(const auto& slot:rig.model.page("effects").subpages)if(slot.effect>=0){
        ++declaredBanks;CHECK(std::size_t(slot.effect)<banks&&bankIds.insert(slot.effect).second);
        if(std::size_t(slot.effect)>=banks)continue;
        juce::StringArray pageIds;std::set<std::size_t> declared;
        const auto base=saturationParameterStart+std::size_t(slot.effect)*saturationParametersPerSlot;
        for(const auto& leaf:slot.routes)if(leaf.id.startsWith("saturation.")){
            pageIds.add(leaf.id);
            for(const auto& row:leaf.rows)for(const auto& section:row)for(const auto& knob:section.knobs){const auto i=std::size_t(knob.index);CHECK(i>=base&&i<base+saturationParametersPerSlot&&declared.insert(i).second);}
        }
        CHECK(pageIds==saturationPageIds&&declared.size()==saturationParametersPerSlot);
        const auto reach=unitReach(NativeModule::saturation,std::uint8_t(slot.effect));CHECK(reach.size()==saturationParametersPerSlot+1);
        for(const auto i:reach)CHECK(i<shownOn.size()&&shownOn[i]==1); // Includes the predecessor SLOT bypass, retained on its original page.
    }
    CHECK(std::size_t(declaredBanks)==banks&&bankIds.size()==banks&&banks==8);
    CHECK(legacyPages==490&&saturationPages==(int)banks*saturationPageIds.size());
    CHECK(pages==490+(int)banks*saturationPageIds.size()&&once==(int)parameterCount&&ids==(int)parameterCount&&strays==0);
    CHECK(eraImages==1);

    for(int bank=0;bank<4;++bank){
        const auto& page=rig.model.page("synth"+juce::String(bank+1));
        const auto it=std::find_if(page.subpages.begin(),page.subpages.end(),[](const auto& p){return p.id=="drum";});
        CHECK(it!=page.subpages.end());if(it==page.subpages.end())continue;CHECK(it->routes.size()==17);
        CHECK(it->routes[0].first==4235+bank);
        for(int pad=0;pad<16;++pad){
            const auto base=std::size_t(3659+bank*144+pad*9);
            CHECK(base+8<parameterCount);if(base+8>=parameterCount)continue;
            const auto* note=u.knob(base+7);const auto* choke=u.knob(base+8);
            CHECK(note!=nullptr&&choke!=nullptr);
            if(note)CHECK(note->getMinimum()==0&&note->getMaximum()==127&&note->getInterval()==1);
            if(choke)CHECK(choke->getMinimum()==0&&choke->getMaximum()==16&&choke->getInterval()==1);
            CHECK(rig.p.control(base+7).getNumSteps()==128&&rig.p.control(base+8).getNumSteps()==17);
            CHECK(rig.p.control(base+7).getText(1.f,64)=="127"&&rig.p.control(base+7).getValueForText("127")==1.f);
            CHECK(rig.p.control(base+8).getText(0.f,64)=="Off"&&rig.p.control(base+8).getValueForText("Off")==0.f);
            CHECK(it->routes[std::size_t(pad+1)].rows.size()==2);
        }
    }
    u.showPage("synth1");u.showSubpage("routes");const auto oldRoutes=u.routesPage();
    u.showSubpage("drum");for(int page=0;page<17;++page)u.showSubpage("drum");
    CHECK(u.routesPage()==oldRoutes);
    for(int bank=0;bank<4;++bank){
        u.showPage("synth"+juce::String(bank+1));u.showSubpage("common");u.showSubpage("drum");
        for(int step=0;step<17&&u.shownKnobs().size()!=1;++step)u.showSubpage("drum");
        const auto source=u.shownKnobs();CHECK(source.size()==1);if(source.size()==1)CHECK(source.front()->index==std::size_t(4235+bank));
        for(int step=0;step<=bank;++step)u.showSubpage("drum");
        const auto shown=u.shownKnobs();CHECK(shown.size()==9);if(shown.size()==9)CHECK(shown.front()->index==std::size_t(3659+bank*144+bank*9));
    }
    for(int bank=0;bank<4;++bank){u.showPage("synth"+juce::String(bank+1));u.showSubpage("common");u.showSubpage("drum");const auto shown=u.shownKnobs();CHECK(shown.size()==9);if(shown.size()==9)CHECK(shown.front()->index==std::size_t(3659+bank*144+bank*9));}
    const juce::StringArray expectedSpaceLabels{"SPACE","SPACE OUTPUT","DECAY 1","DECAY 2","DECAY 3","DECAY 4","DECAY 5","DECAY 6",
                                               "POST EQ 1","POST EQ 2","POST EQ 3","POST EQ 4","POST EQ 5","POST EQ 6"};
    for(const auto& slot:rig.model.page("effects").subpages)
    {
        juce::StringArray labels;
        for(const auto& page:slot.routes)if(page.id.startsWith("space_reverb."))labels.add(page.label);
        CHECK(labels==expectedSpaceLabels);
    }
}

// The studio support spec's RENDER section: GLOBAL's first row ends in RENDER's QUALITY, render quality's knob, which
// reads REALTIME, then HIGHER OFFLINE once the host changes it.
void programmer_render_quality()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    u.showPage("global");
    juce::StringArray titles;for(auto& t:u.shownTitles())titles.add(t.text);
    auto* q=u.knob(758);
    CHECK(titles.joinIntoString(",")=="PERFORMANCE,OUTPUT,RENDER,MACROS"&&q!=nullptr&&q->isShowing()&&q->getComponentID()=="output.render_quality");
    rig.touch(758);
    const auto realtime=u.display->programmer.value;
    rig.p.control(758).setValueNotifyingHost(1.0f);rig.refresh();
    const auto higher=u.display->programmer.value;
    keep("programmer-global-quality",rig.snap());
    std::cout<<"  '"<<realtime<<"' then '"<<higher<<"'\n";
    CHECK(realtime=="QUALITY REALTIME"&&higher=="QUALITY HIGHER OFFLINE");
}

// The prototype's check 42 through the knobs: route 1's target Cutoff takes bank Analog, bank Common takes target
// Level, ON with no source takes LFO 1 and source None turns it off, each one Route edit; one the processor refuses
// (another gesture holds a parameter) says so and shows all five fields as the processor holds them.
void programmer_routes()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    u.showPage("synth1");
    u.showSubpage("routes");
    const auto at=[&](int field){return rig.physical(252+(std::size_t)field);};
    const auto set=[&](int field,double v){if(auto* k=rig.knob(252+(std::size_t)field))k->setValue(v,juce::sendNotificationSync);};
    const auto shownAsHeld=[&]{for(int f=0;f<5;++f){auto* k=rig.knob(252+(std::size_t)f);if(k==nullptr||std::abs(k->getValue()-at(f))>1e-6)return false;}return true;};
    CHECK(rig.knob(254)!=nullptr&&rig.knob(254)->isShowing());
    rig.reported.clear();
    set(2,14.0);
    const auto bank=at(3)==1.0;
    set(3,0.0);
    const auto target=at(2)==1.0;
    set(0,1.0);
    const auto enable=at(1)==3.0&&at(0)==1.0;
    set(1,0.0);
    const auto disable=at(0)==0.0;
    std::cout<<"  "<<rig.reported.joinIntoString(" | ")<<'\n';
    CHECK(bank&&target&&enable&&disable&&shownAsHeld());
    CHECK(rig.reported.size()==4&&std::all_of(rig.reported.begin(),rig.reported.end(),[](const juce::String& s){return s=="Route edit complete";}));
    CHECK(rig.p.beginParameterGesture(40));
    const auto before=std::array<double,5>{at(0),at(1),at(2),at(3),at(4)};
    set(2,14.0);
    CHECK(rig.said()=="Route edit failed; sound preserved");
    CHECK((std::array<double,5>{at(0),at(1),at(2),at(3),at(4)})==before&&shownAsHeld());
    CHECK(rig.p.endParameterGesture(40));
}

// The prototype's check 43: the drawn macros' tapes on GLOBAL read AIR +40, FACET -25, ECHO +60 and GRIP +30; MACRO 1's
// menu assigns the last touched RESO, keeping its depth (BITE +40), Depth -10% makes it BITE +30 and Clear removes it,
// with today's sentences, and offers no assignment for a control today's macro list does not; Edit assignment opens the
// house unit's editor beside the knob, with every target today's list offers, where an exact depth is one edit.
void programmer_macros()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    rig.drawn();
    rig.touch(40);
    u.showPage("global");
    const auto tapeOf=[&](std::size_t i){for(auto& t:u.shownTapes())if(t.index==i)return t.text;return juce::String();};
    const auto drawnTapes=tapeOf(1)+"|"+tapeOf(2)+"|"+tapeOf(3)+"|"+tapeOf(4);
    std::cout<<"  the macro tapes read "<<drawnTapes<<'\n';
    CHECK(drawnTapes=="AIR +40|FACET -25|ECHO +60|GRIP +30");
    auto* m1=rig.knob(1);
    CHECK(m1!=nullptr);
    if(m1==nullptr)return;
    const juce::String reso(parameterRegistry()[40].name);
    const auto items=menuText(*m1);
    std::cout<<"  MACRO 1's menu: "<<items<<'\n';
    CHECK(items=="Lock|Default|Assign to "+reso+"|Depth +10%|Depth -10%|Clear|Edit assignment");
    rig.reported.clear();
    CHECK(perform(*m1,"Assign to "+reso)&&tapeOf(1)=="BITE +40"&&rig.said()=="Macro assignment complete");
    CHECK(document(rig.p).macros[0].targetKey==parameterRegistry()[40].stateKey);
    CHECK(perform(*m1,"Depth -10%")&&tapeOf(1)=="BITE +30");
    CHECK(perform(*m1,"Clear")&&tapeOf(1).isEmpty()&&rig.said()=="Clear macro complete"&&document(rig.p).macros[0].targetKey==0);
    CHECK(menuText(*m1)=="Lock|Default|Assign to "+reso+"|Edit assignment");
    rig.touch(2);
    CHECK(menuText(*m1)=="Lock|Default|Edit assignment");
    auto& e=u.assign;
    CHECK(perform(*m1,"Edit assignment")&&e.isVisible()&&e.macro==1&&e.getComponentID()=="programmer.assign");
    keep("programmer-assign",rig.snap());
    CHECK(e.target.getComponentID()=="programmer.assign.target"&&e.depth.getComponentID()=="programmer.assign.depth");
    CHECK(!e.getBounds().intersects(m1->getBounds())&&u.getLocalBounds().contains(e.getBounds()));
    juce::StringArray offered;
    for(int i=0;i<e.target.getNumItems();++i)offered.add(e.target.getItemText(i));
    int missing=0;
    const auto r=parameterRegistry();
    for(std::size_t n=0;n<r.size();++n)
        if(r[n].kind==ParameterKind::continuous&&(n==0||n>=9)&&!offered.contains(juce::String(r[n].name)))++missing;
    CHECK(missing==0);
    int air=0;
    for(int i=0;i<e.target.getNumItems();++i)if(e.target.getItemText(i)==juce::String(r[39].name))air=e.target.getItemId(i);
    CHECK(air!=0);
    e.target.setSelectedId(air,juce::sendNotificationSync);
    CHECK(rig.said()=="Macro assignment complete"&&document(rig.p).macros[0].targetKey==r[39].stateKey);
    const auto edits=rig.reported.size();
    e.depth.setValue(-12.5,juce::sendNotificationSync);
    const auto b=document(rig.p).macros[0];
    CHECK(rig.reported.size()==edits+1&&rig.said()=="Macro assignment complete"&&b.targetKey==r[39].stateKey&&std::abs(b.amount+0.125f)<1e-6f);
}

// The prototype's check 44: in the drawn state SYNTH 1's ANALOG has a tape under each control Ember Reed names, drawn
// over the panel and as wide as its name's ink by the skin's rule; WAVETABLE, whose controls no rack names, has none.
void programmer_tapes()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    rig.drawn();
    const auto& tw=rig.model.programmer["tape"]["width"];
    juce::StringArray names;
    float worst=0;
    const auto shot=rig.snap();
    const auto& plate=rig.model.plates.at("advanced");
    double faintest=1000.0;
    for(auto& t:u.shownTapes())
    {
        names.add(juce::String((int)t.index)+" "+t.text);
        worst=std::max(worst,std::abs(t.body.getWidth()-((float)(double)tw["intercept"]+(float)(double)tw["slope"]*t.inkWidth)));
        faintest=std::min(faintest,std::abs(meanLuma(shot,t.body)-meanLuma(plate,t.body)));
    }
    const auto drawnTapes=names.joinIntoString(", ");
    std::cout<<"  "<<drawnTapes<<"; widths to "<<worst<<" px of the rule; the faintest tape "<<faintest<<" luma from the panel\n";
    CHECK(drawnTapes=="39 AIR, 40 BITE, 42 ATTACK, 45 RELEASE, 47 BLOOM"&&worst<0.6f&&faintest>20.0);
    u.showSubpage("wavetable");
    CHECK(u.shownTapes().empty());
}

// The prototype's check 45: SYNTH 1's ANALOG laid out and lettered as drawn. The row rule puts 21 of the 23 drawn knobs
// within 3 px of where they are drawn (F12.11's programmer_row: it draws NOISE's LEVEL and the first squeezed FILTER ENV
// knob off any rule); each label is centred on its knob as drawn to 2.5 px, and the six titles are placed from their
// sections' edges as drawn to 3 px (F12.11's programmer_titles: 2.50 px).
void programmer_rows()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    rig.drawn();
    const auto drawn=rig.model.programmer["drawn"];
    const auto rectOf=[](const juce::var& v){return Rect((float)(double)v[0],(float)(double)v[1],(float)(double)v[2],(float)(double)v[3]);};
    const auto knobs=u.shownKnobs();
    int placed=0,labels=0,titles=0;
    float worstLabel=0,worstTitle=0;
    juce::String whereLabel;
    juce::StringArray off;
    for(auto& v:*drawn["labels"].getArray())
    {
        const auto box=rectOf(v[1]);
        const Pt centre((float)(double)v[2][0],(float)(double)v[2][1]);
        ProgrammerKnob* best=nullptr;
        for(auto* k:knobs)
            if(k->label.getText()==v[0].toString()&&(best==nullptr||k->centre().getDistanceFrom(centre)<best->centre().getDistanceFrom(centre)))best=k;
        if(best==nullptr){off.add(v[0].toString()+" missing");continue;}
        if(const auto dx=std::abs(best->centre().x-centre.x);dx>3.0f){off.add(v[0].toString()+" "+juce::String(dx,2)+" px");continue;}
        ++placed;++labels;
        const auto ink=best->labelInk();
        const float e[]{std::abs((ink.getCentreX()-best->centre().x)-(box.getCentreX()-centre.x)),std::abs(ink.getBottom()-box.getBottom()),
                        std::abs(ink.getHeight()-box.getHeight())};
        if(const auto w=std::max({e[0],e[1],e[2]});w>worstLabel)
        {
            worstLabel=w;
            whereLabel=v[0].toString()+" (centre "+juce::String(e[0],2)+", bottom "+juce::String(e[1],2)+", height "+juce::String(e[2],2)+")";
        }
    }
    for(auto& v:*drawn["titles"].getArray())
    {
        const auto box=rectOf(v[1]);
        const auto drawnLeft=(float)(double)v[2];
        for(auto& t:u.shownTitles())
            if(t.text==v[0].toString()&&std::abs(t.ink.getY()-box.getY())<20.0f)
            {
                ++titles;
                worstTitle=std::max({worstTitle,std::abs((t.ink.getX()-t.left)-(box.getX()-drawnLeft)),std::abs(t.ink.getBottom()-box.getBottom()),
                                     std::abs(t.ink.getHeight()-box.getHeight())});
            }
    }
    std::cout<<"  "<<placed<<" of "<<drawn["labels"].size()<<" drawn knobs within 3 px (off: "<<off.joinIntoString(", ")<<"); labels to "<<worstLabel
             <<" px, "<<whereLabel<<"; "<<titles<<" titles to "<<worstTitle<<" px\n";
    CHECK(placed>=(int)drawn["labels"].size()-2&&labels>=10&&worstLabel<=2.5f&&titles==6&&worstTitle<=3.0f);
}

// The prototype's check 46 in the product's drawn state, where Ember Reed's effect bank holds its drive: FX 1's DRIVE
// and SLOT titled in the title grey and its other kinds in the retained grey, its bank line naming the kind, its place
// and its rack; an unused bank keeps its kind and reads INACTIVE BANK with every kind retained, and a retired one RETIRED.
void programmer_effects()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    rig.drawn();
    u.showPage("effects");
    const auto titleGrey=(int)rig.model.programmer["styles"]["title"]["grey"],retained=(int)rig.model.programmer["styles"]["title"]["retained_grey"];
    const auto greyOf=[&](const juce::String& t){for(auto& x:u.shownTitles())if(x.text==t)return x.grey;return -1;};
    const auto greys=[&]{return juce::String(greyOf("SLOT"))+","+juce::String(greyOf("DRIVE"))+","+juce::String(greyOf("DELAY"))+","+juce::String(greyOf("REVERB"))
                                +","+juce::String(greyOf("COMPRESSOR"));};
    const auto fx1=greys(),bank1=u.bankLine();
    const auto inkLuma=[&](const juce::String& t){const auto shot=rig.snap();for(auto& x:u.shownTitles())if(x.text==t)return meanLuma(shot,x.ink);return 0.0;};
    const auto driveLuma=inkLuma("DRIVE"),delayLuma=inkLuma("DELAY");
    u.showSubpage("fx4");
    const auto bank4=u.bankLine();
    u.showSubpage("fx8");
    const auto fx8=greys(),bank8=u.bankLine();
    auto d=document(rig.p);
    d.layout.effects[7].use=SlotUse::retired;
    u.refresh(d);
    const auto retired=u.bankLine();
    std::cout<<"  FX 1 '"<<bank1<<"' ("<<fx1<<"; DRIVE's ink "<<driveLuma<<", DELAY's "<<delayLuma<<"); '"<<bank4<<"'; '"<<bank8<<"' ("<<fx8<<"); '"<<retired<<"'\n";
    const auto own=[&](int a,int b){return juce::String(titleGrey)+","+juce::String(a)+","+juce::String(b)+","+juce::String(retained)+","+juce::String(retained);};
    CHECK(fx1==own(titleGrey,retained)&&driveLuma>delayLuma+10.0);
    CHECK(bank1=="FX 1  DRIVE  SYNTH 1 INSERT  OWNED BY EMBER REED"&&bank4=="FX 4  COMPRESSOR  MASTER  OWNED BY THREAD PRESS");
    CHECK(fx8==own(retained,retained)&&bank8=="FX 8  DRIVE  INACTIVE BANK"&&retired=="FX 8  DRIVE  RETIRED");
}

// The studio support spec's names on the PROGRAMMER: an EQ bank's bank line and display sentence name it; RACK LAYOUT's
// KIND preserves the predecessor 17-kind order, appends Saturation, and recalls the bank's kind.
void programmer_effect_names()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    rig.drawn();
    rig.addEffect(5,masterChain,EffectKind::eq);
    u.showPage("effects");u.showSubpage("fx6");
    auto* h=u.display!=nullptr?u.display->getAccessibilityHandler():nullptr;
    auto* v=h!=nullptr?h->getValueInterface():nullptr;
    const auto said=v!=nullptr?v->getCurrentValueAsString():juce::String("none");
    std::cout<<"  '"<<u.bankLine()<<"' / '"<<said<<"'\n";
    CHECK(u.bankLine()=="FX 6  EQ  MASTER");
    CHECK(said=="Physical effect 6 / EQ / Master / Present. Change type or owner in Rack layout. CUTOFF 1.80 kHz");
    u.showPage("layout");
    auto* kind=widget<juce::ComboBox>(u,"effect.6.kind");
    CHECK(kind!=nullptr);if(kind==nullptr)return;
    juce::StringArray items;for(int k=0;k<kind->getNumItems();++k)items.add(kind->getItemText(k));
    const auto legacyKinds=juce::StringArray::fromTokens("Drive,Delay,Reverb,Compressor,EQ,Filter,Chorus,Phaser,Flanger,Stereo,Dimension,Frequency shift,Convolution,Frequency split,Mid/side split,Dynamic EQ,Era Reverb",",","");
    auto filtered=items;filtered.removeString("Saturation");CHECK(legacyKinds.size()==17&&filtered==legacyKinds);
    CHECK(items.size()==legacyKinds.size()+1&&items[legacyKinds.size()]=="Saturation");
    u.refresh(document(rig.p),true);
    CHECK(kind->getSelectedId()==5&&kind->getText()=="EQ");
}

// The studio support spec's stepped FX pages, with FX 6 an EQ, FX 7 a phaser and FX 8 a frequency split on the shared
// master: FX N opens on its kind's page, that title in the title grey and the other kinds retained; FX N again steps
// through the declared FX pages, wrapping, and SYNTH 2's ROUTES keeps its page. A kind replaced in RACK LAYOUT opens on its
// own page from EFFECTS; a retired bank opens on its kind's page with every title retained.
void programmer_fx_pages()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    rig.drawn();
    rig.addEffect(5,masterChain,EffectKind::eq);rig.addEffect(6,masterChain,EffectKind::phaser);rig.addEffect(7,masterChain,EffectKind::frequencySplit);
    const auto titles=[&]{juce::StringArray s;for(auto& t:u.shownTitles())s.add(t.text);return s.joinIntoString(",");};
    const auto greyOf=[&](const juce::String& t){for(auto& x:u.shownTitles())if(x.text==t)return x.grey;return -1;};
    const auto titleGrey=(int)rig.model.programmer["styles"]["title"]["grey"],retained=(int)rig.model.programmer["styles"]["title"]["retained_grey"];
    const juce::String page1("SLOT,DRIVE,DELAY,REVERB,COMPRESSOR"),page2("EQ,FILTER,CHORUS"),page3("PHASER,FLANGER,STEREO,DIMENSION"),
                       page4("FREQUENCY SHIFT,FREQUENCY SPLIT,CONVOLUTION,MID/SIDE SPLIT");
    u.showPage("synth2");
    for(int i=0;i<2;++i)if(auto* b=rig.subpageButton("ROUTES"))click(*b);
    const auto routes=u.routesPage();
    u.showPage("effects");
    juce::StringArray opened;std::vector<int> own;
    const std::pair<const char*,const char*> banks[]{{"fx6","EQ"},{"fx7","PHASER"},{"fx8","FREQUENCY SPLIT"}};
    for(const auto& [sub,kind]:banks)
    {
        u.showSubpage(sub);opened.add(titles());own.push_back(greyOf(kind));
        keep(juce::String("programmer-effects-")+sub,rig.snap());
    }
    u.showSubpage("fx6");
    const auto others=juce::String(greyOf("FILTER"))+","+juce::String(greyOf("CHORUS"));
    auto* low=u.knob(804);
    const auto lowShown=low!=nullptr&&low->isShowing();
    juce::StringArray steps;
    const auto pageCount=rig.model.page("effects").subpages[5].routes.size();
    for(std::size_t i=0;i<pageCount;++i){u.showSubpage("fx6");steps.add(titles());}
    u.showSubpage("fx1");const auto drive=titles();
    u.showSubpage("fx6");const auto back=titles();
    u.showSubpage("fx6");
    u.showPage("synth2");const auto routesAfter=u.routesPage();
    u.showPage("effects");const auto reopened=titles();
    std::cout<<"  opened '"<<opened.joinIntoString("' '")<<"', stepped '"<<steps.joinIntoString("' '")<<"'; ROUTES "<<routes<<" then "<<routesAfter<<'\n';
    const std::vector<int> ownGrey(3,titleGrey);
    CHECK(opened==juce::StringArray(page2,page3,page4)&&own==ownGrey);
    CHECK(others==juce::String(retained)+","+juce::String(retained)&&lowShown);
    juce::StringArray expectedSteps(page3,page4);
    for(int band=0;band<8;++band)expectedSteps.add((band==0?juce::String("DYNAMIC EQ"):"DYNAMIC EQ BAND "+juce::String(band+1))+",CHANNEL / DYNAMICS");
    expectedSteps.add("DYNAMIC EQ OUTPUT");expectedSteps.add("DYNAMIC EQ GAIN");for(int band=0;band<8;++band)expectedSteps.add("DETECTOR");
    for(int band=0;band<8;++band)expectedSteps.add("SPECTRAL");
    expectedSteps.add("DELAY TAPE");expectedSteps.add("DELAY STEREO");
    expectedSteps.add("SPACE REVERB");expectedSteps.add("SPACE OUTPUT / DUCK");
    for(int band=1;band<=6;++band)expectedSteps.add("DECAY "+juce::String(band));
    for(int band=1;band<=6;++band)expectedSteps.add("POST EQ "+juce::String(band));
    expectedSteps.add("ERA REVERB");expectedSteps.add("ERA TONE");
    expectedSteps.add(page1);expectedSteps.add(page2);
    const auto legacySteps=expectedSteps;
    const juce::StringArray saturationSteps{"SATURATION,INPUT / OUTPUT,CROSSOVERS,WET CUTS","FULL CHARACTER,FULL BLEND","LOW CHARACTER,LOW BLEND","MID CHARACTER,MID BLEND","HIGH CHARACTER,HIGH BLEND"};
    expectedSteps.clear();
    for(int i=0;i<legacySteps.size();++i){if(i==legacySteps.size()-2)for(const auto& title:saturationSteps)expectedSteps.add(title);expectedSteps.add(legacySteps[i]);}
    juce::StringArray seenLegacy,seenSaturation;
    for(const auto& title:steps)(saturationSteps.contains(title)?seenSaturation:seenLegacy).add(title);
    CHECK(legacySteps.size()==48&&seenLegacy==legacySteps);
    CHECK(saturationSteps.size()==5&&seenSaturation==saturationSteps);
    CHECK(pageCount==std::size_t(legacySteps.size()+saturationSteps.size())&&steps==expectedSteps&&drive==page1&&back==page2&&reopened==page2&&routes==1&&routesAfter==1);
    for(std::size_t slot=0;slot<8;++slot){
        const auto& pages=rig.model.page("effects").subpages[slot].routes;
        const auto stereo=std::find_if(pages.begin(),pages.end(),[](const auto& page){return page.id=="delay.modes";});
        CHECK(stereo!=pages.end());if(stereo==pages.end())continue;
        const int first=int(2191+3*slot);
        CHECK(stereo->label=="DELAY STEREO"&&stereo->first==first&&stereo->last==first+2&&stereo->rows.size()==1&&stereo->rows[0].size()==1);
        if(stereo->rows.size()!=1||stereo->rows[0].size()!=1)continue;
        const auto& knobs=stereo->rows[0][0].knobs;
        CHECK(knobs.size()==3&&knobs[0].label=="RIGHT"&&knobs[0].index==first&&knobs[1].label=="PING-PONG"&&knobs[1].index==first+1&&knobs[2].label=="NOTE"&&knobs[2].index==first+2);
    }

    u.showPage("layout");
    click(*u.rackRows()[9]);
    if(auto* kind=widget<juce::ComboBox>(u,"effect.6.kind"))kind->setSelectedId(15,juce::sendNotificationSync);
    if(auto* replace=widget<juce::Button>(u,"effect.6.replace"))click(*replace);
    u.showPage("effects");
    const auto replaced=titles();const auto splitGrey=greyOf("MID/SIDE SPLIT");
    auto d=document(rig.p);
    d.layout.effects[5].use=SlotUse::retired;
    u.refresh(d);
    u.showSubpage("fx1");u.showSubpage("fx6");
    const auto retiredTitles=titles();
    bool allRetained=true;for(auto& t:u.shownTitles())allRetained=allRetained&&t.grey==retained;
    std::cout<<"  replaced '"<<replaced<<"', retired '"<<retiredTitles<<"', '"<<u.bankLine()<<"'\n";
    CHECK(document(rig.p).layout.effects[5].kind==EffectKind::midSideSplit&&replaced==page4&&splitGrey==titleGrey);
    CHECK(retiredTitles==page4&&allRetained&&u.bankLine()=="FX 6  MID/SIDE SPLIT  RETIRED");
}

// The product's: in the drawn state the display reads SYNTH 1's bank line and CUTOFF's value, lit on its glass; the bank
// line squeezes, then elides, short of the value line, its accessible value the full text; GLOBAL, RACK LAYOUT, ROUTES,
// the engine banks and an unused or retired synth bank read as decision 6 has them; on FX N the accessible value is
// today's effect status sentence.
void programmer_display()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    rig.drawn();
    auto* d=u.display.get();
    CHECK(d!=nullptr);
    if(d==nullptr)return;
    CHECK(d->getComponentID()=="programmer.display"&&d->getBounds()==juce::Rectangle<int>(94,147,1358,67));
    std::cout<<"  '"<<d->programmer.bank<<"' / '"<<d->programmer.value<<"'\n";
    CHECK(d->programmer.bank=="SYNTH 1  ANALOG  ACTIVE  OWNED BY EMBER REED"&&d->programmer.value=="CUTOFF 1.80 kHz");
    CHECK(d->placed.count("bank")==1&&d->placed.count("value")==1);
    if(d->placed.count("bank")==1&&d->placed.count("value")==1)
    {
        const auto& plate=rig.model.plates.at("advanced");
        const auto shot=rig.snap();
        const auto bankInk=d->placed["bank"].path.getBounds(),valueInk=d->placed["value"].path.getBounds();
        CHECK(d->placed["bank"].shown==d->programmer.bank&&d->placed["value"].shown=="CUTOFF 1.80 kHz");
        CHECK(meanLuma(shot,bankInk)>meanLuma(plate,bankInk)+20.0&&meanLuma(shot,valueInk)>meanLuma(plate,valueInk)+20.0);
    }
    auto* h=d->getAccessibilityHandler();
    const auto accessible=[&]{auto* v=h!=nullptr?h->getValueInterface():nullptr;return v!=nullptr?v->getCurrentValueAsString():juce::String("none");};
    CHECK(accessible()=="SYNTH 1  ANALOG  ACTIVE  OWNED BY EMBER REED. CUTOFF 1.80 kHz");
    // A rack with a longer name: the bank line squeezes short of the value line, then elides; the accessible value keeps it.
    const auto renamed=[&](const juce::String& name)
    {
        auto doc=document(rig.p);
        auto state=*doc.personalities;
        state.instances[0].design.name=name;
        doc.personalities=std::make_shared<const RackPersonalityState>(state);
        u.refresh(doc);
    };
    const auto gap=(float)(double)rig.model.displays.at("advanced").spec["gap"];
    const auto room=[&]{return d->placed["value"].path.getBounds().getX()-gap-d->placed["bank"].path.getBounds().getRight();};
    const auto drawnScale=d->placed["bank"].hscale;
    renamed("Ember Reed Of The Long Winter Evenings");
    const auto squeezed=d->placed["bank"];
    std::cout<<"  drawn at "<<drawnScale<<", squeezed to "<<squeezed.hscale<<" ("<<room()<<" px to spare): "<<squeezed.shown<<'\n';
    CHECK(squeezed.shown==d->programmer.bank&&squeezed.hscale<drawnScale-0.01f&&room()>=-0.5f);
    renamed("Ember Reed Of The Long Winter Evenings By The Quiet Northern Lake Shore");
    const auto elided=d->placed["bank"];
    std::cout<<"  elided at "<<elided.hscale<<" ("<<room()<<" px to spare): "<<elided.shown<<'\n';
    CHECK(elided.shown.endsWith(juce::String::charToString((juce::juce_wchar)0x2026))&&elided.shown.length()<d->programmer.bank.length()&&room()>=-0.5f);
    CHECK(accessible()=="SYNTH 1  ANALOG  ACTIVE  OWNED BY EMBER REED OF THE LONG WINTER EVENINGS BY THE QUIET NORTHERN LAKE SHORE. CUTOFF 1.80 kHz");
    rig.refresh();
    juce::StringArray lines;
    const auto read=[&](const juce::String& page,const juce::String& sub){u.showPage(page);if(sub.isNotEmpty())u.showSubpage(sub);lines.add(d->programmer.bank);};
    read("global","");read("layout","");read("synth1","routes");read("synth1","wavetable");read("synth2","wavetable");read("synth2","analog");
    read("synth2","lfo");read("synth3","common");
    auto doc=document(rig.p);
    doc.layout.synthUse[2]=SlotUse::retired;
    u.refresh(doc);
    lines.add(d->programmer.bank);
    std::cout<<"  "<<lines.joinIntoString(" | ")<<'\n';
    CHECK(lines.joinIntoString("|")=="GLOBAL  SHARED BY EVERY RACK|RACK LAYOUT  2 SYNTHS  5 EFFECTS|SYNTH 1  ROUTES 1-4  ON  OWNED BY EMBER REED|"
                                     "SYNTH 1  WAVETABLE  RETAINED  OWNED BY EMBER REED|SYNTH 2  WAVETABLE  ACTIVE  OWNED BY GLASS CURRENT|"
                                     "SYNTH 2  ANALOG  RETAINED  OWNED BY GLASS CURRENT|SYNTH 2  LFO 1/2  ON  OWNED BY GLASS CURRENT|SYNTH 3  COMMON  UNUSED|"
                                     "SYNTH 3  COMMON  RETIRED");
    rig.refresh();
    u.showPage("effects");
    u.showSubpage("fx1");
    const juce::String status("Physical effect 1 / Drive / Synth 1 insert / Present. Change type or owner in Rack layout.");
    CHECK(u.effectStatus[0].getComponentID()=="effect.1.status"&&u.effectStatus[0].getText()==status&&!u.effectStatus[0].isVisible());
    CHECK(accessible()==status+" CUTOFF 1.80 kHz");
}

// The prototype's check 47 for the unit: the page and sub-page buttons are toggles, checked while their page shows.
void programmer_unit_roles()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    rig.drawn();
    CHECK(u.pageButtons.size()==7);
    if(u.pageButtons.size()<2)return;
    auto* p0=u.pageButtons[0]->getAccessibilityHandler();
    auto* p1=u.pageButtons[1]->getAccessibilityHandler();
    CHECK(p0!=nullptr&&p1!=nullptr&&p1->getRole()==juce::AccessibilityRole::toggleButton&&p1->getCurrentState().isChecked()&&!p0->getCurrentState().isChecked());
    auto* common=rig.subpageButton("COMMON");
    auto* analog=rig.subpageButton("ANALOG");
    auto* c=common!=nullptr?common->getAccessibilityHandler():nullptr;
    auto* a=analog!=nullptr?analog->getAccessibilityHandler():nullptr;
    CHECK(c!=nullptr&&a!=nullptr&&a->getRole()==juce::AccessibilityRole::toggleButton&&a->getCurrentState().isChecked()&&!c->getCurrentState().isChecked());
    if(common!=nullptr)click(*common);
    CHECK(c!=nullptr&&a!=nullptr&&c->getCurrentState().isChecked()&&!a->getCurrentState().isChecked()&&u.subpage()=="common");
}

// The product's: a new unit opens on GLOBAL, lit, with no value line; a page shows the sub-page it showed last, one not
// shown yet its first; the value line follows the last touched control wherever it is, host automation too.
void programmer_view_state()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    CHECK(u.page()=="global"&&rig.litPages()=="GLOBAL"&&u.touched()==nullptr);
    CHECK(u.display!=nullptr&&u.display->programmer.value.isEmpty()&&u.display->programmer.bank=="GLOBAL  SHARED BY EVERY RACK");
    u.showPage("synth1");
    CHECK(u.subpage()=="common");
    u.showSubpage("wavetable");
    u.showPage("global");
    u.showPage("synth1");
    CHECK(u.subpage()=="wavetable");
    u.showPage("synth2");
    CHECK(u.subpage()=="common");
    u.showPage("effects");
    u.showSubpage("fx3");
    u.showPage("layout");
    u.showPage("effects");
    CHECK(u.subpage()=="fx3"&&rig.subpages().startsWith("FX 1,FX 2,FX 3*,"));
    rig.touch(39);
    CHECK(u.touched()==u.knob(39)&&u.display!=nullptr&&u.display->programmer.value==u.knob(39)->valueLine());
    auto& c=rig.p.control(39);
    c.setValueNotifyingHost(c.convertTo0to1(440.0f));
    rig.refresh();
    CHECK(u.display!=nullptr&&u.display->programmer.value=="CUTOFF 440 Hz");
}

// The product's RACK LAYOUT (decision 10, as Task 4 settles it): the twelve rows read today's row sentence in the
// PROGRAMMER's form, with today's sentence as their description. A row selects its bank, whose controls show under the
// synth banks with today's IDs, classes, behaviour and sentences. Every bank's controls fit there at 1x, apart and with
// their legends whole; the tab order runs down the rows, then the selected bank's controls.
void programmer_rack_layout()
{
    section(__func__);
    UnitRig rig;
    auto& u=*rig.unit;
    u.showPage("layout");
    const auto rows=u.rackRows();
    CHECK(rows.size()==12);
    if(rows.size()!=12)return;
    const auto prefix=[](std::size_t i){return juce::String(i<4?"synth.":"effect.")+juce::String((int)(i<4?i:i-4)+1);};
    const auto texts=[&]{juce::StringArray s;for(auto* r:rows)s.add(r->getButtonText());return s.joinIntoString("|");};
    const auto chosen=[&]{juce::StringArray s;for(std::size_t i=0;i<12;++i)if(rows[i]->getToggleState())s.add(prefix(i));return s.joinIntoString(",");};
    const auto shows=[&](const juce::String& id){auto* c=byId(u,id);return c!=nullptr&&c->isShowing();};
    const auto press=[&](const juce::String& id){auto* b=widget<juce::Button>(u,id);CHECK(b!=nullptr&&b->isShowing());if(b!=nullptr)click(*b);};
    const auto choose=[&](const juce::String& id,int item)
    {auto* c=widget<juce::ComboBox>(u,id);CHECK(c!=nullptr&&c->isShowing());if(c!=nullptr)c->setSelectedId(item,juce::sendNotificationSync);};
    const auto stripTitle=[&]{const auto t=u.shownTitles();return t.size()==3?t[2].text:juce::String();};
    const char* const actions[]{"add","remove","replace","up","down"};
    const char* const controls[]{"kind","owner","add","remove","replace","up","down","move","lock"};
    bool named=true;
    for(std::size_t i=0;i<12;++i)
    {
        const auto p=prefix(i);
        named=named&&rows[i]->getComponentID()==p+".row"&&rows[i]->getName()=="Physical bank "+juce::String((int)(i<4?i:i-4)+1);
        for(auto* a:actions){auto* b=widget<juce::TextButton>(u,p+"."+a);named=named&&b!=nullptr&&b->getName()==p+" "+a;}
        auto* lock=widget<juce::ToggleButton>(u,p+".lock");
        named=named&&lock!=nullptr&&lock->getName()==p+" lock";
        if(i<4){named=named&&byId(u,p+".move")==nullptr&&byId(u,p+".kind")==nullptr&&byId(u,p+".owner")==nullptr;continue;}
        auto* move=widget<juce::TextButton>(u,p+".move");
        auto* kind=widget<juce::ComboBox>(u,p+".kind");
        auto* owner=widget<juce::ComboBox>(u,p+".owner");
        named=named&&move!=nullptr&&move->getName()==p+" move"&&kind!=nullptr&&kind->getName()=="Replacement effect type"
              &&owner!=nullptr&&owner->getName()=="Destination chain";
    }
    CHECK(named);
    CHECK(texts()=="SYNTH 1  PRESENT  POSITION 1|SYNTH 2  UNUSED|SYNTH 3  UNUSED|SYNTH 4  UNUSED|EFFECT 1  UNUSED|EFFECT 2  UNUSED|"
                   "EFFECT 3  UNUSED|EFFECT 4  UNUSED|EFFECT 5  UNUSED|EFFECT 6  UNUSED|EFFECT 7  UNUSED|EFFECT 8  UNUSED");
    CHECK(rows[0]->getDescription()=="Synth 1 physical bank / Present / position 1"&&rows[5]->getDescription()=="Effect 2 physical bank / Unused");
    CHECK(chosen()=="synth.1"&&stripTitle()=="SYNTH 1"&&shows("synth.1.add")&&!shows("synth.2.add")&&!shows("effect.1.add"));

    // A synth bank: ADD, UP and DOWN, LOCK; REMOVE retires it, ADD then says to use REPLACE, which brings it back
    // with its values and locks.
    click(*rows[1]);
    CHECK(chosen()=="synth.2"&&stripTitle()=="SYNTH 2"&&shows("synth.2.add")&&!shows("synth.1.add"));
    press("synth.2.add");
    CHECK(document(rig.p).layout.synthUse[1]==SlotUse::present&&rig.said()=="Rack edit complete"&&rows[1]->getButtonText()=="SYNTH 2  PRESENT  POSITION 2");
    press("synth.2.up");
    CHECK(document(rig.p).layout.synthOrder[0]==1&&rows[1]->getButtonText()=="SYNTH 2  PRESENT  POSITION 1"&&rows[0]->getButtonText()=="SYNTH 1  PRESENT  POSITION 2");
    press("synth.2.down");
    CHECK(document(rig.p).layout.synthOrder[1]==1&&rig.said()=="Rack edit complete");
    press("synth.2.lock");
    auto* lock=widget<juce::ToggleButton>(u,"synth.2.lock");
    CHECK(document(rig.p).synthLocks[1]&&rig.said()=="Rack lock complete"&&lock!=nullptr&&lock->getToggleState());
    {
        auto d=document(rig.p);
        d.controlLocks[idx("synth.2.level")]=true;d.values[idx("synth.2.level")]=0.8f;
        CHECK(edit(rig.p,d));rig.refresh();
    }
    const auto retained=document(rig.p);
    press("synth.2.remove");
    CHECK(document(rig.p).layout.synthUse[1]==SlotUse::retired&&rig.said()=="Rack edit complete");
    CHECK(rows[1]->getButtonText()=="SYNTH 2  RETIRED  REPLACE TO REUSE"&&rows[1]->getDescription()=="Synth 2 physical bank / Retired - Replace to reuse");
    press("synth.2.add");
    CHECK(document(rig.p).layout.synthUse[1]==SlotUse::retired&&rig.said()=="Physical bank 2 retains prior intent. Use its explicit Replace button.");
    press("synth.2.replace");
    const auto back=document(rig.p);
    CHECK(back.layout.synthUse[1]==SlotUse::present&&back.values==retained.values&&back.controlLocks==retained.controlLocks&&rig.said()=="Rack edit complete");

    // An effect bank: ADD to its OWNER, KIND then REPLACE, UP and DOWN, MOVE to the chosen OWNER, LOCK, REMOVE, ADD
    // refused and REPLACE; REMOVE on a bank no rack uses fails with the sound preserved.
    click(*rows[6]);
    CHECK(chosen()=="effect.3"&&stripTitle()=="EFFECT 3"&&shows("effect.3.kind")&&shows("effect.3.owner")&&shows("effect.3.move")&&!shows("synth.2.add"));
    press("effect.3.add");
    auto l=document(rig.p).layout;
    CHECK(l.effects[2].use==SlotUse::present&&l.effects[2].chain==masterChain&&l.effectOrder[masterChain][0]==2&&rig.said()=="Rack edit complete");
    CHECK(rows[6]->getButtonText()=="EFFECT 3  PRESENT  MASTER  POSITION 1"&&rows[6]->getDescription()=="Effect 3 physical bank / Present / Master / position 1");
    choose("effect.3.kind",2);
    press("effect.3.replace");
    CHECK(document(rig.p).layout.effects[2].kind==EffectKind::delay&&rig.said()=="Rack edit complete");
    click(*rows[7]);
    press("effect.4.add");
    press("effect.4.up");
    l=document(rig.p).layout;
    CHECK(l.effectOrder[masterChain][0]==3&&l.effectOrder[masterChain][1]==2&&rows[7]->getButtonText()=="EFFECT 4  PRESENT  MASTER  POSITION 1");
    press("effect.4.down");
    CHECK(document(rig.p).layout.effectOrder[masterChain][0]==2&&rig.said()=="Rack edit complete");
    click(*rows[6]);
    choose("effect.3.owner",2);
    press("effect.3.move");
    CHECK(document(rig.p).layout.effects[2].chain==1&&rows[6]->getButtonText()=="EFFECT 3  PRESENT  SYNTH 2  POSITION 1");
    press("effect.3.lock");
    CHECK(document(rig.p).effectLocks[2]&&rig.said()=="Rack lock complete");
    const auto kept=document(rig.p);
    press("effect.3.remove");
    CHECK(document(rig.p).layout.effects[2].use==SlotUse::retired&&rows[6]->getButtonText()=="EFFECT 3  RETIRED  REPLACE TO REUSE");
    press("effect.3.add");
    CHECK(document(rig.p).layout.effects[2].use==SlotUse::retired&&rig.said()=="Physical bank 3 retains prior intent. Use its explicit Replace button.");
    press("effect.3.replace");
    CHECK(document(rig.p).layout.effects[2].use==SlotUse::present&&document(rig.p).values==kept.values&&rig.said()=="Rack edit complete");
    click(*rows[11]);
    const auto unused=document(rig.p);
    press("effect.8.remove");
    CHECK(document(rig.p)==unused&&rig.said()=="Rack edit failed; sound preserved");

    // Each bank's controls fit under the synth banks at 1x: inside the strip with the fields' legends, apart and clear
    // of its title, the keys' legends and the fields' options whole; the rows keep to their columns, apart.
    const auto& P=rig.model.programmer;
    const auto num=[](const juce::var& v){return (float)(double)v;};
    const auto left=num(P["body"]["x"][0]),right=num(P["body"]["x"][1]),mid=std::round((left+right)*0.5f);
    const auto strip=Rect::leftTopRightBottom(left,num(P["body"]["rows"][1]["divider"][0]),mid-2.0f,num(P["body"]["rows"][1]["divider"][1]));
    const ProgrammerLook look(rig.model);
    const auto onUnit=[&](juce::Component& c){return u.getLocalArea(c.getParentComponent(),c.getBounds()).toFloat();};
    const auto whole=[&](const TextStyle& style,const juce::String& text,float width)
    {const auto s=setText(look.faces,style,text.toUpperCase(),0.0f,0.0f,width,0.8f);return s.shown==text.toUpperCase()&&s.hscale>=style.hscale;};
    int placed=0,outside=0,overlaps=0,squeezed=0,rowsOff=0;
    for(std::size_t i=0;i<12;++i)
    {
        click(*rows[i]);
        std::vector<juce::Component*> shown;
        for(auto* name:controls)
            if(auto* c=byId(u,prefix(i)+"."+name);c!=nullptr&&c->isShowing())shown.push_back(c);
        const auto t=u.shownTitles();
        const auto title=t.size()==3?t[2].ink:Rect();
        if(title.isEmpty()||!strip.contains(title))++outside;
        for(std::size_t a=0;a<shown.size();++a)
        {
            ++placed;
            auto area=onUnit(*shown[a]);
            auto* box=dynamic_cast<juce::ComboBox*>(shown[a]);
            if(box!=nullptr)area=area.withTop(area.getY()-8.0f-look.legendStyle.cap);
            if(!strip.contains(area)||area.intersects(title))++outside;
            for(std::size_t b=0;b<a;++b)if(onUnit(*shown[b]).intersects(onUnit(*shown[a])))++overlaps;
            if(auto* button=dynamic_cast<juce::Button*>(shown[a]);button!=nullptr&&!whole(look.legendStyle,button->getButtonText(),(float)button->getWidth()))++squeezed;
            if(box!=nullptr)
                for(int k=0;k<box->getNumItems();++k)if(!whole(look.optionStyle,box->getItemText(k),(float)box->getWidth()-34.0f))++squeezed;
        }
    }
    for(std::size_t i=0;i<12;++i)
    {
        const auto r=onUnit(*rows[i]);
        const auto column=i<4?Rect::leftTopRightBottom(left,0.0f,mid-2.0f,strip.getY()):Rect::leftTopRightBottom(mid+2.0f,0.0f,right,strip.getBottom());
        if(!column.contains(r))++rowsOff;
        for(std::size_t j=0;j<i;++j)if(onUnit(*rows[j]).intersects(r))++rowsOff;
    }
    std::cout<<"  "<<placed<<" bank controls: "<<outside<<" outside the strip, "<<overlaps<<" overlapping, "<<squeezed<<" squeezed; "<<rowsOff<<" rows off\n";
    CHECK(placed==4*6+8*9&&outside==0&&overlaps==0&&squeezed==0&&rowsOff==0);

    // The tab order: down the rows, then the selected bank's controls.
    click(*rows[6]);
    juce::StringArray order,want;
    for(auto* c:u.createKeyboardFocusTraverser()->getAllComponents(&u))
        if(const auto id=c->getComponentID();id.startsWith("synth.")||id.startsWith("effect."))order.add(id);
    for(std::size_t i=0;i<12;++i)want.add(prefix(i)+".row");
    for(auto* name:controls)want.add(juce::String("effect.3.")+name);
    if(order!=want)std::cout<<"  tab order "<<order.joinIntoString(",")<<'\n';
    CHECK(order==want);

    // The selection is view state, kept across pages; the rows are toggles, checked while selected. A pending KIND
    // stays through a refresh, as today's does, and a recalled sound resets it to the bank's.
    u.showPage("global");
    u.showPage("layout");
    auto* h=rows[6]->getAccessibilityHandler();
    CHECK(chosen()=="effect.3"&&shows("effect.3.add")&&h!=nullptr&&h->getRole()==juce::AccessibilityRole::toggleButton&&h->getCurrentState().isChecked());
    auto* kindBox=widget<juce::ComboBox>(u,"effect.3.kind");
    choose("effect.3.kind",3);
    rig.refresh();
    const auto pending=kindBox!=nullptr?kindBox->getSelectedId():0;
    u.refresh(document(rig.p),true);
    const auto reset=kindBox!=nullptr?kindBox->getSelectedId():0;
    CHECK(pending==3&&reset==(int)document(rig.p).layout.effects[2].kind+1);
    keep("programmer-layout-effect3",rig.snap());
}
// Decision 2's panel: a unit's keys and views over exactly the parameters userReach gives it; ROUTES pressed again
// turns its four pages; the same units keep their knobs.
void panel_units()
{
    section(__func__);
    using U=AllControlsPanel::Unit;
    {
        PanelRig rig{{{NativeModule::analog,0}}};
        auto& u=*rig.panel;
        CHECK(u.keys()==(std::vector<juce::String>{"COMMON","ANALOG","LFO 1/2","ROUTES"}));
        CHECK(u.parameters()==unitReach(NativeModule::analog,0));
        const auto common=rig.shown();
        CHECK(u.shownKey()==0&&common.size()==5&&std::find(common.begin(),common.end(),13)==common.end());
        u.showKey(1);const auto engine=rig.shown().size();
        u.showKey(2);const auto lfo=rig.shown().size();
        std::cout<<"  COMMON "<<common.size()<<", ANALOG "<<engine<<", LFO 1/2 "<<lfo<<" knobs\n";
        CHECK(engine==23&&lfo==8);
        std::vector<int> firsts;
        for(int n=0;n<5;++n){auto* b=rig.key(3);CHECK(b!=nullptr);if(b!=nullptr)click(*b);const auto s=rig.shown();firsts.push_back(s.size()==20?(int)s.front():-1);}
        CHECK(firsts==(std::vector<int>{252,272,292,312,252})&&u.shownKey()==3);
        auto* cutoff=u.knob(39);
        u.show({{NativeModule::analog,0}});
        CHECK(cutoff!=nullptr&&u.knob(39)==cutoff);
    }
    {
        PanelRig rig{{{NativeModule::wavetable,1},{NativeModule::delay,1},{NativeModule::reverb,2}}};
        auto& u=*rig.panel;
        const auto keys=u.keys();
        CHECK(keys.size()==7&&keys[5]=="FX 2"&&keys[6]=="FX 3");
        std::vector<std::size_t> all;
        for(const auto& [m,b]:std::initializer_list<std::pair<NativeModule,std::uint8_t>>{{NativeModule::wavetable,1},{NativeModule::delay,1},{NativeModule::reverb,2}})
            for(const auto i:unitReach(m,b))all.push_back(i);
        std::sort(all.begin(),all.end());
        CHECK(all.size()==147&&u.parameters()==all);
        u.showKey(5);
        const auto fx2=rig.shown();
        CHECK(fx2.size()==16&&fx2.front()==idx("effect.2.bypassed"));
    }
    {
        PanelRig rig{std::vector<U>{}};
        for(int m=0;m<17;++m)
        {
            rig.panel->show({{NativeModule(m),0}});
            if(rig.panel->parameters()!=unitReach(NativeModule(m),0)){CHECK(false);std::cout<<"  "<<moduleDisplayName(NativeModule(m))<<"'s panel differs from its reach\n";}
        }
    }
    RackInstance copper;copper.design=airack::test::frozenFactoryRackDesigns()[0];copper.bindings={{0,2}};
    CHECK(panelUnits(copper)==(std::vector<U>{{NativeModule::analog,2}}));
}
// A revision-2 wavetable unit's WARMTH key follows WAVETABLE and holds exactly its slot's nine warmth keys; a
// revision-1 unit, whose kernel ignores them, has no WARMTH key.

void panel_drum_key() {
    section(__func__);
    auto owned=std::make_unique<PanelRig>(std::vector<AllControlsPanel::Unit>{{NativeModule::drum,0,1}});auto& rig=*owned;auto& panel=*rig.panel;
    rig.p.control(drumModeParameterStart).setValue(1);rig.refresh();
    CHECK(panel.keys()==(std::vector<juce::String>{"COMMON","DRUM"}));CHECK(panel.parameters()==unitReach(NativeModule::drum,0,1)&&panel.parameters().size()==147);
    CHECK(!panel.knob(13)&&!panel.knob(drumModeParameterStart));CHECK(rig.shown().size()==3);keep("drum-drawer-common",rig.snap());
    panel.showKey(1);std::set<std::size_t> seen;
    for(int pad=0;pad<16;++pad){const auto shown=rig.shown();CHECK(shown.size()==9);CHECK(!shown.empty()&&shown.front()==drumParameterStart+pad*9);for(auto i:shown)seen.insert(i);
        if(pad==0||pad==15){
            auto* note=rig.knob(drumParameterStart+pad*9+7);auto* choke=rig.knob(drumParameterStart+pad*9+8);CHECK(note&&choke);
            if(note&&choke){for(const auto& text:juce::StringArray{"0","127"}){note->openEntry();note->entry.setText(text);note->commitEntry();CHECK(rig.physical(note->index)==text.getDoubleValue());}for(const auto& text:juce::StringArray{"Off","16"}){choke->openEntry();choke->entry.setText(text);choke->commitEntry();CHECK(rig.physical(choke->index)==(text=="Off"?0:16));}}
            keep("drum-drawer-pad-"+juce::String(pad+1),rig.snap());
        }
        auto* key=rig.key(1);CHECK(key);if(key)click(*key);
    }
    CHECK(seen.size()==144&&rig.shown().front()==drumParameterStart);
    for(const auto [module,revision,count]:{std::tuple{NativeModule::analog,AlgorithmRevision(1),116u},std::tuple{NativeModule::wavetable,AlgorithmRevision(1),116u},std::tuple{NativeModule::wavetable,AlgorithmRevision(2),125u},std::tuple{NativeModule::eraReverb,AlgorithmRevision(1),16u}}){panel.show({{module,0,revision}});CHECK(panel.parameters().size()==count);}
    RackInstance drum;drum.design.units={{0,NativeModule::drum,{},1}};drum.bindings={{0,3}};CHECK(panelUnits(drum)==(std::vector<AllControlsPanel::Unit>{{NativeModule::drum,3,1}}));
}
void panel_warmth_key()
{
    section(__func__);
    {
        PanelRig rig{{{NativeModule::wavetable,2,modernSynthAlgorithmRevision}}};
        auto& u=*rig.panel;
        CHECK(u.keys()==(std::vector<juce::String>{"COMMON","WAVETABLE","WARMTH","LFO 1/2","ROUTES"}));
        u.showKey(2);auto shown=rig.shown();std::sort(shown.begin(),shown.end());
        std::vector<std::size_t> block(wavetableWarmthParametersPerSlot);
        std::iota(block.begin(),block.end(),wavetableWarmthParameterStart+2*wavetableWarmthParametersPerSlot);
        CHECK(shown==block);
    }
    {
        PanelRig rig{{{NativeModule::wavetable,2,legacyAlgorithmRevision}}};
        CHECK(rig.panel->keys()==(std::vector<juce::String>{"COMMON","WAVETABLE","LFO 1/2","ROUTES"}));
        const auto p=rig.panel->parameters();
        CHECK(std::none_of(p.begin(),p.end(),[](std::size_t i){return i>=wavetableWarmthParameterStart;}));
    }
}

// The panel as tall as its page (F14.5): COMMON's one row ends 20 px under its dividers, at 345 px, and ANALOG's two at
// panelHeight; every shown knob and name lies inside the height; onHeight fires when the height changes and not when
// ROUTES turns to a page as tall.
void panel_height()
{
    section(__func__);
    PanelRig rig{{{NativeModule::analog,0}}};
    auto& u=*rig.panel;
    int changes=0;u.onHeight=[&]{++changes;};
    CHECK(u.shownKey()==0&&u.shownHeight()==345);
    u.showKey(1);
    CHECK(u.shownHeight()==AllControlsPanel::panelHeight&&changes==1);
    u.showKey(0);
    CHECK(u.shownHeight()==345&&changes==2);
    bool inside=true;
    const auto check=[&](const char* page)
    {
        int bottom=0;
        for(auto* k:u.shownKnobs())bottom=std::max({bottom,k->getBottom(),k->label.getBottom()});
        std::cout<<"  "<<page<<": "<<u.shownHeight()<<" px, its knobs and names to "<<bottom<<'\n';
        inside=inside&&bottom<=u.shownHeight();
    };
    check("COMMON");u.showKey(1);check("ANALOG");u.showKey(2);check("LFO 1/2");u.showKey(3);check("ROUTES 1");
    const auto before=changes;
    for(const auto* page:{"ROUTES 2","ROUTES 3","ROUTES 4"}){u.showKey(3);check(page);}
    CHECK(inside&&changes==before);
}

// The panel's knobs edit as the PROGRAMMER's do: macro assignment from each continuous knob's menu keeping the macro's
// depth, today's route edit, the locks, and the last touched knob's value line.
void panel_edits()
{
    section(__func__);
    PanelRig rig{{{NativeModule::analog,0}}};
    auto& u=*rig.panel;
    auto* cutoff=rig.knob(39);auto* reso=rig.knob(40);auto* source=rig.knob(253);
    CHECK(cutoff!=nullptr&&reso!=nullptr&&source!=nullptr);
    if(cutoff==nullptr||reso==nullptr||source==nullptr)return;
    auto* line=widget<juce::Label>(u,"rack.controls.line");
    CHECK(line!=nullptr&&line->getText().isEmpty());
    juce::StringArray assign;for(int n=1;n<=8;++n)assign.add("Assign to MACRO "+juce::String(n));
    CHECK(menuText(*cutoff)=="Lock|Default|"+assign.joinIntoString("|"));
    CHECK(menuText(*source)=="Lock|Default");
    rig.reported.clear();
    CHECK(perform(*cutoff,"Assign to MACRO 3")&&rig.said()=="Macro assignment complete");
    auto m=document(rig.p).macros[2];
    CHECK(m.targetKey==parameterRegistry()[39].stateKey&&m.amount==0.5f);
    {auto d=document(rig.p);d.macros[2].amount=-0.25f;CHECK(edit(rig.p,d));rig.refresh();}
    CHECK(perform(*reso,"Assign to MACRO 3"));
    m=document(rig.p).macros[2];
    CHECK(m.targetKey==parameterRegistry()[40].stateKey&&m.amount==-0.25f);

    u.showKey(3);
    const auto at=[&](int field){return rig.physical(252+(std::size_t)field);};
    const auto set=[&](int field,double v){if(auto* k=rig.knob(252+(std::size_t)field))k->setValue(v,juce::sendNotificationSync);};
    const auto shownAsHeld=[&]{for(int f=0;f<5;++f){auto* k=rig.knob(252+(std::size_t)f);if(k==nullptr||std::abs(k->getValue()-at(f))>1e-6)return false;}return true;};
    CHECK(rig.knob(254)!=nullptr&&rig.knob(254)->isShowing());
    rig.reported.clear();
    set(2,14.0);
    const auto bank=at(3)==1.0;
    set(3,0.0);
    const auto target=at(2)==1.0;
    set(0,1.0);
    const auto enable=at(1)==3.0&&at(0)==1.0;
    set(1,0.0);
    const auto disable=at(0)==0.0;
    std::cout<<"  "<<rig.reported.joinIntoString(" | ")<<'\n';
    CHECK(bank&&target&&enable&&disable&&shownAsHeld());
    CHECK(rig.reported.size()==4&&std::all_of(rig.reported.begin(),rig.reported.end(),[](const juce::String& s){return s=="Route edit complete";}));
    CHECK(rig.p.beginParameterGesture(40));
    const auto before=std::array<double,5>{at(0),at(1),at(2),at(3),at(4)};
    set(2,14.0);
    CHECK(rig.said()=="Route edit failed; sound preserved");
    CHECK((std::array<double,5>{at(0),at(1),at(2),at(3),at(4)})==before&&shownAsHeld());
    CHECK(rig.p.endParameterGesture(40));

    CHECK(perform(*cutoff,"Lock")&&rig.said()=="Control lock complete"&&cutoff->isLocked());
    u.showKey(1);
    rig.touch(39);
    CHECK(line!=nullptr&&line->getText()==cutoff->valueLine()&&line->getText().startsWith("CUTOFF "));
}

// The panel's marks: each changed knob the candidate holds, its pointer dashed at the proposed turn where it shows, and
// the key round a view holding a mark it doesn't show, ROUTES' while the mark is on another page of four.
void panel_marks()
{
    section(__func__);
    PanelRig rig{{{NativeModule::analog,0}}};
    auto& u=*rig.panel;
    auto candidate=document(rig.p);
    candidate.values[39]=candidate.values[39]<0.5f?0.9f:0.1f;
    candidate.values[296]=candidate.values[296]<0.5f?0.75f:0.25f;
    const auto proposed=[&](std::size_t i){return rig.p.control(i).convertFrom0to1(candidate.values[i]);};
    const auto marked=[](const juce::Image& shot,const std::vector<Pt>& along){int n=0;for(const auto& q:along){const auto c=shot.getPixelAt((int)std::floor(q.x),(int)std::floor(q.y));n+=std::min({c.getRed(),c.getGreen(),c.getBlue()})>=200?1:0;}return n;};
    const auto outline=[&](int n){std::vector<Pt> v;if(auto* b=rig.key(n)){const auto r=b->getBounds().toFloat().expanded(3.0f);for(float x=r.getX()+2.0f;x<r.getRight()-2.0f;x+=1.0f)v.push_back({x,r.getY()});}return v;};
    const auto pointer=[&](ProgrammerKnob& k,float degrees){std::vector<Pt> v;const auto a=juce::degreesToRadians(degrees);const auto c=k.getBounds().toFloat().getCentre();
        for(float t=0.2f;t<=0.95f;t+=0.01f)v.push_back(c+Pt(std::sin(a),-std::cos(a))*((float)k.getWidth()*0.5f*t));return v;};
    const auto bare=rig.snap();
    u.showProposal(std::make_shared<const SoundDocument>(candidate));
    CHECK(u.marks()==(std::vector<AllControlsPanel::Mark>{{39,proposed(39)},{296,proposed(296)}}));
    CHECK(u.outlinedKeys()==(std::vector<int>{1,3}));
    const auto withMarks=rig.snap();
    const auto keyAfter=marked(withMarks,outline(1)),keyBefore=marked(bare,outline(1));
    u.showKey(1);
    CHECK(u.outlinedKeys()==(std::vector<int>{3}));
    auto* cutoff=rig.knob(39);
    CHECK(cutoff!=nullptr);
    if(cutoff==nullptr)return;
    const auto angle=cutoff->angleAt(proposed(39));
    const auto pointed=marked(rig.snap(),pointer(*cutoff,angle));
    u.showProposal(nullptr);
    const auto unpointed=marked(rig.snap(),pointer(*cutoff,angle));
    u.showProposal(std::make_shared<const SoundDocument>(candidate));
    std::cout<<"  key 1's outline: "<<keyAfter<<" marked pixels, "<<keyBefore<<" bare; CUTOFF's pointer at "<<angle<<" degrees: "<<pointed<<" marked, "<<unpointed<<" bare\n";
    CHECK(keyAfter>=30&&keyBefore==0&&pointed>=20&&pointed>=unpointed+15);
    u.showKey(3);
    const auto page0=u.outlinedKeys();
    u.showKey(3);u.showKey(3);
    CHECK(page0==(std::vector<int>{1,3})&&u.routesPage()==2&&u.outlinedKeys()==(std::vector<int>{1}));
    {auto d=document(rig.p);d.controlLocks[39]=true;CHECK(edit(rig.p,d));rig.refresh();}
    CHECK(u.marks()==(std::vector<AllControlsPanel::Mark>{{296,proposed(296)}}));
    keep("panel-marks",rig.snap());
}

// The house unit's controls are the house parameters userReach counts.
void house_reach()
{
    section("house_reach");
    std::set<std::size_t> bound;
    for(const auto& e:studioSkin()->house.elements)if(e.paramId.isNotEmpty())bound.insert(e.paramIndex);
    const auto house=houseParameters();
    CHECK(bound==std::set<std::size_t>(house.begin(),house.end()));
}
}

int main(int argc,char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if(argc==2&&juce::String(argv[1])=="--numeric-carets"){
        programmer_numeric_caret_lifetime();panel_numeric_caret_lifetime();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures==0?0:1;
    }
    if(argc==3&&juce::String(argv[1])=="--images"){output=juce::File(argv[2]);CHECK(output.createDirectory());}
    else if(argc!=1)return 2;
    values_format();values_parse();values_round_trip();
    programmer_knobs();programmer_gestures();programmer_roles();programmer_numeric_caret_lifetime();
    programmer_pages();programmer_every_parameter();programmer_render_quality();programmer_routes();programmer_macros();programmer_tapes();programmer_rows();
    programmer_effects();programmer_effect_names();programmer_fx_pages();programmer_display();programmer_unit_roles();programmer_view_state();programmer_rack_layout();house_reach();
    panel_units();panel_drum_key();panel_warmth_key();panel_height();panel_edits();panel_marks();panel_numeric_caret_lifetime();
    std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";
    return failures==0?0:1;
}
