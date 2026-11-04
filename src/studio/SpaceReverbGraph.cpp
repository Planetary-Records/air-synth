#include "SpaceReverbGraph.h"
#include "InstrumentProcessor.h"
#include "ProgrammerLook.h"
#include "UserReach.h"
#include <algorithm>
#include <cmath>

namespace airack::studio {
namespace {
constexpr double logFrequency=6.907755278982137, logTime=5.991464547107982;
const juce::Colour paper(0xffcad1d6),grid(0xff30383b),ink(0xff87d8bd),clipInk(0xffe7b474);
double frequencyAt(juce::Rectangle<float> r,float x){return 20*std::exp(logFrequency*std::clamp(double((x-r.getX())/std::max(1.f,r.getWidth())),0.,1.));}
float xFor(juce::Rectangle<float> r,double f){return r.getX()+r.getWidth()*float(std::log(std::clamp(f,20.,20000.)/20)/logFrequency);}
float timeY(juce::Rectangle<float> r,double t){return r.getBottom()-r.getHeight()*float(std::log(std::clamp(t,.1,40.)/.1)/logTime);}
double timeAt(juce::Rectangle<float> r,float y){return .1*std::exp(logTime*std::clamp(double((r.getBottom()-y)/std::max(1.f,r.getHeight())),0.,1.));}
float gainY(juce::Rectangle<float> r,double db){return r.getCentreY()-r.getHeight()*float(std::clamp(db,-24.,24.)/48);}
double gainAt(juce::Rectangle<float> r,float y){return std::clamp(double((r.getCentreY()-y)/std::max(1.f,r.getHeight()))*48,-18.,18.);}
double decibels(std::complex<double> h){return 20*std::log10(std::max(std::abs(h),1.e-12));}
bool hasGain(space::PostShape s){return s<=space::PostShape::highShelf;}
juce::Uuid rackOwner(const SoundDocument& d,std::uint8_t slot){
    if(d.personalities)for(const auto& i:d.personalities->instances)for(const auto& u:i.design.units)
        if(u.module==NativeModule::spaceReverb)if(const auto* b=rackUnitBinding(i,u.id);b&&b->slot==slot)return i.id;
    return juce::Uuid::null();
}
struct GraphLook final:juce::LookAndFeel_V4 {
    juce::Colour foreground=paper,trace=ink;
    float fontHeight=14;
    juce::Font getComboBoxFont(juce::ComboBox&) override{return juce::Font(juce::FontOptions(fontHeight));}
    void drawButtonBackground(juce::Graphics& g,juce::Button& b,const juce::Colour&,bool over,bool down) override{
        const auto r=b.getLocalBounds().toFloat().reduced(1);if(b.getToggleState()||over||down){g.setColour(trace.withAlpha(down?.24f:b.getToggleState()?.14f:.06f));g.fillRoundedRectangle(r,3);}
        if(ProgrammerLook::ringed(b)){g.setColour(juce::Colour(0xffb3d4ff));g.drawRoundedRectangle(r.reduced(2),2,1.5f);}
    }
    void drawButtonText(juce::Graphics& g,juce::TextButton& b,bool,bool) override{
        g.setFont(juce::FontOptions(fontHeight));g.setColour((b.getToggleState()?trace:foreground).withAlpha(b.isEnabled()?1.f:.35f));g.drawFittedText(b.getButtonText(),b.getLocalBounds().reduced(3,1),juce::Justification::centred,1);
    }
};
}
struct SpaceReverbGraph::State:std::enable_shared_from_this<State> {
    SpaceReverbGraph& view;InstrumentProcessor& processor;house::Host host;GraphLook look;
    KeyFocus<juce::TextButton> decay{"Decay"},post{"Post EQ"},add{"Add"},active{"Active"},bypass{"Bypass"},remove{"Delete"},numeric{"Controls"};
    KeyFocus<juce::ComboBox> choice;
    SoundDocument sound;space::Settings settings,base;std::optional<space::Settings> proposal;
    std::shared_ptr<const SoundDocument> candidate;
    std::optional<std::uint8_t> slot;Mode mode=Mode::decay;std::array<std::size_t,2> selected{};
    struct Context {SoundDocument document;std::uint64_t revision=0,reset=0,binding=0;std::uint8_t slot=0;};
    std::shared_ptr<const Context> expected;
    space::Settings dragBase;
    std::uint64_t binding=0,observedReset=0;
    std::size_t leader=parameterCount;
    double rate=48000;
    bool retired=false,dragging=false,inCall=false,abort=false,resetKnown=false,marked=false,maker=false;
    State(SpaceReverbGraph& v,InstrumentProcessor& p,house::Host h):view(v),processor(p),host(std::move(h)){
        int order=1;
        for(auto* b:std::array<juce::TextButton*,7>{&decay,&post,&add,&active,&bypass,&remove,&numeric}){
            if(b==&add)++order;
            b->setLookAndFeel(&look);b->setExplicitFocusOrder(order++);b->setComponentID("space."+b->getButtonText().toLowerCase().replaceCharacter(' ','.'));view.addAndMakeVisible(b);
        }
        decay.setTooltip("Frequency-dependent decay target in seconds. D selects this view.");post.setTooltip("Wet output amplitude response in dB. P selects this view.");
        add.setTooltip("Activate the next unused fixed band. Retained settings are preserved.");remove.setTooltip("Deactivate this band, retaining every setting. Delete also removes it.");
        active.setTooltip("Activate or deactivate this fixed band, retaining its settings.");bypass.setTooltip("Bypass this band's contribution, retaining its settings.");numeric.setTooltip("Open the selected band's numeric controls. Enter also opens them.");
        choice.setLookAndFeel(&look);choice.setComponentID("space.band");choice.setTitle("Selected fixed band identity");choice.setExplicitFocusOrder(3);view.addAndMakeVisible(choice);
        for(int n=1;n<=6;++n)choice.addItem("Band "+juce::String(n),n);
        decay.onClick=[this]{if(!retired&&!closing())view.setMode(Mode::decay);};post.onClick=[this]{if(!retired&&!closing())view.setMode(Mode::postEq);};
        choice.onChange=[this]{if(!retired&&!closing())view.selectBand(std::size_t(choice.getSelectedId()-1));};
        add.onClick=[this]{addBand({});};active.onClick=[this]{toggle(0);};bypass.onClick=[this]{toggle(1);};
        remove.onClick=[this]{setField(0,0);};numeric.onClick=[this]{notifySelection();};
    }
    std::shared_ptr<void> frame() const{return host.frame?host.frame():nullptr;}
    bool closing() const{return host.closing&&host.closing();}
    std::size_t chosen() const{return selected[std::size_t(mode)];}
    std::size_t field(std::size_t n,std::size_t offset) const{return spaceReverbParameterStart+std::size_t(*slot)*spaceReverbParametersPerSlot+(mode==Mode::decay?12+n*6:48+n*8)+offset;}
    bool bandActive(std::size_t n) const{return mode==Mode::decay?base.decay.bands[n].active:base.post[n].active;}
    bool bandBypassed(std::size_t n) const{return mode==Mode::decay?base.decay.bands[n].bypassed:base.post[n].bypassed;}
    void report(){if(!retired&&!closing()&&host.report)host.report("Sound changed or another edit is active; sound preserved");}
    void changed(){if(!retired&&!closing()){const auto callback=view.onEdited;if(callback)callback();}}
    void notifySelection(){const auto keep=shared_from_this();const auto scope=frame();if(!capture())return;const auto callback=view.onBandSelected;if(callback)callback(mode,chosen());}
    void end(){
        const auto keep=shared_from_this();if(inCall){abort=true;return;}expected.reset();if(!dragging)return;
        const auto scope=frame();const auto index=leader;dragging=false;abort=false;inCall=true;const bool ok=processor.endParameterGesture(index);inCall=false;if(!ok)report();
    }
    std::shared_ptr<const Context> capture(){
        const auto keep=shared_from_this();const auto scope=frame();if(retired||closing()||!slot)return {};
        auto c=std::make_shared<Context>();c->slot=*slot;c->binding=binding;
        if(!processor.trySoundSnapshot(c->document,c->revision,c->reset))return {};
        if((resetKnown&&c->reset!=observedReset)||c->document!=sound)return {};
        return c;
    }
    bool current(const Context& c) const{
        if(retired||closing()||!slot||*slot!=c.slot||binding!=c.binding)return false;
        SoundDocument d;std::uint64_t revision,reset;
        return processor.trySoundSnapshot(d,revision,reset)&&revision==c.revision&&reset==c.reset&&d==c.document;
    }
    bool begin(const std::shared_ptr<const Context>& c,std::span<const ParameterGestureValue> edits){
        const auto keep=shared_from_this();const auto scope=frame();if(!c||edits.empty()||!current(*c))return false;
        end();if(retired||closing()||!slot)return false;leader=edits.front().index;expected=c;abort=false;
        inCall=true;const bool ok=processor.beginParameterGesture(leader,edits,c->document,c->revision,c->reset);inCall=false;dragging=ok;
        if(!ok||retired||closing()||abort){end();if(!ok)report();return false;}return true;
    }
    bool write(std::span<const ParameterGestureValue> edits){
        const auto keep=shared_from_this();const auto scope=frame();const auto c=expected;
        if(!dragging||!c||retired||closing()){end();return false;}
        inCall=true;const bool ok=processor.setParameterGestureValues(leader,edits,c->document,c->revision,c->reset);inCall=false;
        if(!ok||retired||closing()||abort){end();if(!ok)report();return false;}
        auto next=std::make_shared<Context>();next->slot=c->slot;next->binding=c->binding;
        if(!processor.trySoundSnapshot(next->document,next->revision,next->reset)||next->reset!=c->reset||!slot||*slot!=c->slot||binding!=c->binding){end();return false;}
        expected=next;view.refresh(next->document,next->slot);changed();return !retired&&!closing()&&!abort;
    }
    bool execute(const std::shared_ptr<const Context>& c,std::span<const ParameterGestureValue> edits){
        const auto keep=shared_from_this();const auto scope=frame();if(!begin(c,edits))return false;const bool ok=write(edits);end();return ok;
    }
    ParameterGestureValue physical(std::size_t i,double value) const{return {i,processor.control(i).convertTo0to1(float(value))};}
    void setField(std::size_t offset,float normalized){const auto keep=shared_from_this();const auto scope=frame();const auto c=capture();if(!c)return;const ParameterGestureValue edit{field(chosen(),offset),normalized};(void)execute(c,std::span(&edit,1));}
    void toggle(std::size_t offset){const auto keep=shared_from_this();if(!slot||retired)return;setField(offset,offset==0?!bandActive(chosen()):!bandBypassed(chosen()));}
    std::vector<ParameterGestureValue> nodeValues(const space::Settings& source,std::size_t n,juce::Point<float> at) const{
        const auto r=view.plotBounds();const double hz=frequencyAt(r,at.x);std::vector<ParameterGestureValue> edits{physical(field(n,3),hz)};
        if(mode==Mode::decay){const auto multiplier=view.multiplierForTarget(source.decay,n,hz,timeAt(r,at.y),rate);if(multiplier)edits.push_back(physical(field(n,4),*multiplier));}
        else if(hasGain(source.post[n].shape))edits.push_back(physical(field(n,4),gainAt(r,at.y)));
        return edits;
    }
    bool beginNode(){const auto c=capture();if(!c)return false;dragBase=base;const auto edits=nodeValues(dragBase,chosen(),view.nodePosition(chosen()));return begin(c,edits);}
    void moveNode(juce::Point<float> at){if(dragging){const auto edits=nodeValues(dragBase,chosen(),at);(void)write(edits);}}
    void addBand(std::optional<juce::Point<float>> at){
        const auto keep=shared_from_this();const auto scope=frame();const auto c=capture();if(!c)return;
        std::size_t n=0;while(n<6&&bandActive(n))++n;if(n==6)return;
        std::vector<ParameterGestureValue> edits{{field(n,0),1}};
        if(at){const auto values=nodeValues(base,n,*at);edits.insert(edits.end(),values.begin(),values.end());}
        if(execute(c,edits)&&!retired&&!closing())view.selectBand(n);
    }
    void wheelQ(double delta){
        const auto keep=shared_from_this();const auto scope=frame();const auto c=capture();if(!c)return;
        const auto n=chosen();std::size_t offset=5;double q=base.decay.bands[n].q,upper=4;
        if(mode==Mode::postEq){const auto& b=base.post[n];if(!hasGain(b.shape))return;offset=b.shape==space::PostShape::bell?5:6;q=offset==5?b.bellQ:b.shelfQ;upper=offset==5?12:4;}
        const auto edit=physical(field(n,offset),std::clamp(q*std::exp(delta),.25,upper));(void)execute(c,std::span(&edit,1));
    }
    void sync(){
        if(retired||!slot)return;const auto n=chosen();const bool isActive=bandActive(n);
        decay.setToggleState(mode==Mode::decay,juce::dontSendNotification);post.setToggleState(mode==Mode::postEq,juce::dontSendNotification);
        choice.setSelectedId(int(n+1),juce::dontSendNotification);active.setToggleState(isActive,juce::dontSendNotification);bypass.setToggleState(bandBypassed(n),juce::dontSendNotification);
        remove.setEnabled(isActive);bool unused=false;for(std::size_t b=0;b<6;++b)unused=unused||!bandActive(b);add.setEnabled(unused);
        view.setTitle("FX "+juce::String(int(*slot+1))+" Space Reverb "+(mode==Mode::decay?"decay graph":"wet post-EQ graph"));
        view.setDescription("Six fixed band identities. Drag frequency and "+juce::String(mode==Mode::decay?"effective decay seconds":"wet gain")+"; wheel or + / - adjusts Q. D / P switches view, 1 to 6 selects a band, A toggles active, B toggles bypass, Delete removes, Enter opens numeric controls. Arrow keys move the selected node, Shift for fine steps. Frequencies above the logical sample-rate limit remain retained.");
    }
    void updateProposal(){
        marked=false;proposal.reset();if(!slot||!candidate||sound.effectLocks[*slot]||rackOwner(*candidate,*slot)!=rackOwner(sound,*slot))return;
        auto admitted=std::make_unique<SoundDocument>(*candidate);for(std::size_t i=0;i<parameterCount;++i)if(sound.controlLocks[i])admitted->values[i]=sound.values[i];
        RoutedPatch p;if(!makeRoutedPatch(*admitted,p)||p.layout.effects[*slot].use!=SlotUse::present||p.layout.effects[*slot].kind!=EffectKind::spaceReverb)return;
        for(const auto i:unitReach(NativeModule::spaceReverb,*slot))marked=marked||std::abs(admitted->values[i]-sound.values[i])>1.e-6f;
        if(marked)proposal=p.effects[*slot].spaceReverb;
    }
};
SpaceReverbGraph::SpaceReverbGraph(InstrumentProcessor& p,house::Host h):state(std::make_shared<State>(*this,p,std::move(h))){setComponentID("space.reverb.graph");setWantsKeyboardFocus(true);setExplicitFocusOrder(9);}
SpaceReverbGraph::~SpaceReverbGraph(){retire();}
std::optional<double> SpaceReverbGraph::multiplierForTarget(space::DecayCurve curve,std::size_t n,double hz,double seconds,double rate){
    if(n>=6||!std::isfinite(seconds)||seconds<=0||!space::valid(curve))return {};
    auto& b=curve.bands[n];b.active=true;b.bypassed=false;b.frequencyHz=hz;b.multiplier=1;
    const auto one=space::evaluate(curve,hz,rate);b.multiplier=std::exp(1.);const auto e=space::evaluate(curve,hz,rate);
    if(!one||!e||one->unclampedSeconds<=0||e->unclampedSeconds<=0)return {};
    const double sensitivity=std::log(e->unclampedSeconds/one->unclampedSeconds);
    if(!std::isfinite(sensitivity)||sensitivity<=0)return {};
    return std::clamp(std::exp(std::log(seconds/one->unclampedSeconds)/sensitivity),.25,4.);
}
void SpaceReverbGraph::refresh(const SoundDocument& d,std::uint8_t slot){
    const auto keep=state;auto& s=*keep;const auto scope=s.frame();if(s.retired||s.closing())return;
    SoundDocument live;std::uint64_t revision,reset;if(s.processor.trySoundSnapshot(live,revision,reset)){const bool recalled=s.resetKnown&&reset!=s.observedReset;s.observedReset=reset;s.resetKnown=true;if(recalled){++s.binding;s.end();}}
    if(s.retired||s.closing())return;
    if(slot>=8||d.layout.effects[slot].use!=SlotUse::present||d.layout.effects[slot].kind!=EffectKind::spaceReverb){++s.binding;s.end();if(s.retired)return;s.slot.reset();setEnabled(false);return;}
    if(s.slot&&(*s.slot!=slot||s.sound.layout!=d.layout||rackOwner(s.sound,*s.slot)!=rackOwner(d,slot))){++s.binding;s.end();}
    if(s.retired||s.closing())return;
    RoutedPatch p;if(!makeRoutedPatch(d,p))return;s.slot=slot;s.sound=d;s.settings=p.effects[slot].spaceReverb;
    auto raw=std::make_unique<SoundDocument>(d);raw->macros={};if(makeRoutedPatch(*raw,p))s.base=p.effects[slot].spaceReverb;
    const auto actual=s.processor.getSampleRate();const double rate=actual>=8000&&actual<=384000?actual:48000;
    if(rate!=s.rate){s.rate=rate;s.end();if(s.retired||s.closing())return;}
    setEnabled(true);s.updateProposal();s.sync();repaint();
}
void SpaceReverbGraph::showProposal(std::shared_ptr<const SoundDocument> p){if(state->retired)return;state->candidate=std::move(p);state->updateProposal();repaint();}
void SpaceReverbGraph::setMode(Mode m){const auto keep=state;const auto scope=keep->frame();if(keep->retired||keep->closing()||m>Mode::postEq||keep->mode==m)return;keep->end();if(keep->retired||keep->closing())return;keep->mode=m;keep->sync();repaint();}
SpaceReverbGraph::Mode SpaceReverbGraph::mode() const{return state->mode;}
void SpaceReverbGraph::selectBand(std::size_t n){const auto keep=state;const auto scope=keep->frame();if(n>=6||keep->retired||keep->closing())return;keep->end();if(keep->retired||keep->closing())return;keep->selected[std::size_t(keep->mode)]=n;keep->sync();repaint();keep->notifySelection();}
std::size_t SpaceReverbGraph::selectedBand() const{return state->chosen();}
std::optional<std::uint8_t> SpaceReverbGraph::boundSlot() const{return state->slot;}
float SpaceReverbGraph::pixelSize() const{
    if(!state->maker)return 1;
    const auto span=getTopLevelComponent()->getLocalArea(this,juce::Rectangle<int>(0,0,1000,1000)).getWidth();
    return 1000.f/std::max(1,span);
}
int SpaceReverbGraph::toolbarRows() const{return state->maker&&getWidth()/pixelSize()<660?2:1;}
void SpaceReverbGraph::setMakerStyle(bool maker){
    if(state->maker==maker)return;
    state->maker=maker;getProperties().set("maker.theme",maker?"amber_crt":"");
    state->look.foreground=maker?juce::Colour(0xffe9bd77):paper;
    state->look.trace=maker?juce::Colour(0xffffce78):ink;
    state->choice.setColour(juce::ComboBox::backgroundColourId,maker?juce::Colour(0xff241b10):juce::Colour(0xff11191d));
    state->choice.setColour(juce::ComboBox::textColourId,state->look.foreground);
    state->choice.setColour(juce::ComboBox::outlineColourId,maker?juce::Colour(0xff735637):grid);
    resized();repaint();
}
juce::Rectangle<float> SpaceReverbGraph::plotBounds() const{
    if(!state->maker)return {52,56,std::max(1.f,float(getWidth())-86),std::max(1.f,float(getHeight())-103)};
    const auto u=pixelSize(),top=(toolbarRows()*23.f+7)*u;
    return {34*u,top,std::max(u,float(getWidth())-44*u),std::max(u,float(getHeight())-top-27*u)};
}
juce::Point<float> SpaceReverbGraph::nodePosition(std::size_t n) const{
    if(n>=6)return {};const auto& s=*state;const auto r=plotBounds();
    if(s.mode==Mode::decay){const auto& b=s.base.decay.bands[n];const auto point=space::evaluate(s.base.decay,b.frequencyHz,s.rate);return {xFor(r,b.frequencyHz),timeY(r,point?point->seconds:s.base.decay.decaySeconds)};}
    const auto& b=s.base.post[n];return {xFor(r,b.frequencyHz),gainY(r,hasGain(b.shape)?b.gainDb:0)};
}
std::optional<space::DecayPoint> SpaceReverbGraph::decayAt(double hz,bool proposed) const{const auto& s=*state;if(!s.slot||(proposed&&!s.proposal))return {};return space::evaluate(proposed?s.proposal->decay:s.settings.decay,hz,s.rate);}
std::optional<std::complex<double>> SpaceReverbGraph::postEqAt(double hz,bool proposed) const{const auto& s=*state;if(!s.slot||!std::isfinite(hz)||hz<20||hz>20000||(proposed&&!s.proposal))return {};space::PreparedPostEq p;if(!space::preparePostEq(proposed?*s.proposal:s.settings,s.rate,p))return {};return space::postEqResponse(p,std::min(hz,.5*s.rate));}
std::vector<std::size_t> SpaceReverbGraph::parameters() const{std::vector<std::size_t> out;const auto& s=*state;if(s.slot)for(std::size_t n=0;n<6;++n)for(const auto offset:std::array<std::size_t,5>{0,1,3,4,5}){out.push_back(spaceReverbParameterStart+std::size_t(*s.slot)*spaceReverbParametersPerSlot+12+n*6+offset);out.push_back(spaceReverbParameterStart+std::size_t(*s.slot)*spaceReverbParametersPerSlot+48+n*8+offset);}if(s.slot)for(std::size_t n=0;n<6;++n)out.push_back(spaceReverbParameterStart+std::size_t(*s.slot)*spaceReverbParametersPerSlot+48+n*8+6);return out;}
bool SpaceReverbGraph::proposed() const{return state->marked;}
void SpaceReverbGraph::suspend(){const auto keep=state;keep->end();}
void SpaceReverbGraph::retire(){const auto keep=state;if(keep&&!keep->retired){keep->retired=true;keep->end();}}
void SpaceReverbGraph::resized(){
    auto& s=*state;
    if(s.maker){
        const auto u=pixelSize();s.look.fontHeight=12*u;
        const int rows=toolbarRows(),columns=8/rows;int n=0;
        const float width=(getWidth()-4*u)/columns;
        for(auto* c:std::initializer_list<juce::Component*>{&s.decay,&s.post,&s.choice,&s.add,&s.active,&s.bypass,&s.remove,&s.numeric}){
            c->setBounds(juce::Rectangle<float>(2*u+(n%columns)*width,2*u+(n/columns)*23*u,width-2*u,21*u).toNearestInt());++n;
        }
        return;
    }
    s.look.fontHeight=14;int x=52;const auto button=[&](juce::Component& c,int width){c.setBounds(x,7,width,30);x+=width+4;};
    button(s.decay,64);button(s.post,74);button(s.choice,100);button(s.add,48);button(s.active,58);button(s.bypass,60);button(s.remove,54);button(s.numeric,74);
}
void SpaceReverbGraph::paint(juce::Graphics& g){
    auto& s=*state;const auto u=pixelSize();const auto paper=s.look.foreground,ink=s.look.trace;
    const auto grid=s.maker?juce::Colour(0xff4b3920):juce::Colour(0xff30383b);
    g.fillAll(s.maker?juce::Colour(0xff1e170e):juce::Colour(0xff11191d));const auto r=plotBounds();g.setFont(juce::FontOptions((s.maker?11.f:12.f)*u));
    for(const double hz:{20.,100.,1000.,10000.,20000.}){const float x=xFor(r,hz);g.setColour(grid);g.drawVerticalLine(int(x),r.getY(),r.getBottom());g.setColour(paper.withAlpha(.65f));g.drawText(hz>=1000?juce::String(hz/1000,0)+"k":juce::String(hz,0),juce::Rectangle<float>(x-20*u,r.getBottom()+2*u,40*u,12*u),juce::Justification::centred);}
    const auto horizontal=[&](double value){const auto y=s.mode==Mode::decay?timeY(r,value):gainY(r,value);g.setColour(grid);g.drawHorizontalLine(int(y),r.getX(),r.getRight());g.setColour(paper.withAlpha(.65f));g.drawText(juce::String(value,value<1?1:0),juce::Rectangle<float>(u,y-7*u,r.getX()-5*u,14*u),juce::Justification::centredRight);};
    if(s.mode==Mode::decay)for(const double t:{.1,1.,10.,40.})horizontal(t);else for(const double db:{-18.,0.,18.})horizontal(db);
    if(!s.maker){g.setColour(paper.withAlpha(.65f));g.drawText(s.mode==Mode::decay?"seconds":"wet dB",3,37,80,16,juce::Justification::centredLeft);}
    const double limit=std::min(20000.,.45*s.rate);if(limit<20000){const float x=xFor(r,limit);g.setColour(clipInk.withAlpha(.08f));g.fillRect(juce::Rectangle<float>(x,r.getY(),r.getRight()-x,r.getHeight()));g.setColour(clipInk);g.drawVerticalLine(int(x),r.getY(),r.getBottom());}
    if(!s.slot)return;
    bool clipped=false;
    const auto curve=[&](const space::Settings& settings,bool proposed){
        space::PreparedPostEq post;if(s.mode==Mode::postEq&&!space::preparePostEq(settings,s.rate,post))return;
        juce::Path path;for(int i=0;i<=420;++i){const double hz=20*std::exp(logFrequency*i/420.);float y=0;
            if(s.mode==Mode::decay){const auto p=space::evaluate(settings.decay,hz,s.rate);if(!p)return;y=timeY(r,p->seconds);if(!proposed)clipped=clipped||p->decayLimited;}
            else{const auto h=space::postEqResponse(post,std::min(hz,.5*s.rate));if(!h)return;y=gainY(r,decibels(*h));}
            const float x=r.getX()+r.getWidth()*i/420.f;if(i==0)path.startNewSubPath(x,y);else path.lineTo(x,y);
        }
        g.setColour(proposed?paper:ink);if(proposed)ProgrammerLook::drawDashes(g,path,paper,5*u,4*u);else g.strokePath(path,juce::PathStrokeType(1.8f*u));
    };
    curve(s.settings,false);if(s.proposal)curve(*s.proposal,true);
    for(std::size_t n=0;n<6;++n)if(s.bandActive(n)){
        const auto p=nodePosition(n);g.setColour(s.bandBypassed(n)?paper.withAlpha(.45f):ink);g.fillEllipse(p.x-4*u,p.y-4*u,8*u,8*u);
        if(n==s.chosen()){g.setColour(paper);g.drawEllipse(p.x-7*u,p.y-7*u,14*u,14*u,u);}
        g.setColour(paper);g.drawText(juce::String(int(n+1)),juce::Rectangle<float>(p.x+6*u,p.y-16*u,20*u,14*u),juce::Justification::centredLeft);
    }
    juce::String status=s.mode==Mode::decay?"Decay target 0.1-40 s":"Wet output EQ";if(clipped)status+=" | target clipped";
    if(s.settings!=s.base)status+=" | curve includes macros; nodes show retained values";
    if(s.maker)status=juce::String(s.mode==Mode::decay?"Seconds":"Wet dB")+(clipped?" | target clipped":"")+(s.settings!=s.base?" | macros":"");
    status+=" | limit "+juce::String(limit/1000,1)+" kHz";g.setColour(clipped?clipInk:paper.withAlpha(.65f));g.drawText(status,juce::Rectangle<float>(r.getX(),getHeight()-12*u,r.getWidth(),11*u),juce::Justification::centredLeft);
}
void SpaceReverbGraph::mouseDown(const juce::MouseEvent& e){const auto keep=state;const auto scope=keep->frame();if(keep->retired||keep->closing()||!keep->slot||!e.mods.isLeftButtonDown())return;std::optional<std::size_t> nearest;float distance=14;for(std::size_t n=0;n<6;++n)if(keep->bandActive(n)){const auto d=nodePosition(n).getDistanceFrom(e.position);if(d<distance){nearest=n;distance=d;}}if(nearest){selectBand(*nearest);if(keep->retired||keep->closing())return;if(keep->beginNode())grabKeyboardFocus();}}
void SpaceReverbGraph::mouseDrag(const juce::MouseEvent& e){const auto keep=state;const auto scope=keep->frame();if(!keep->retired&&!keep->closing())keep->moveNode(e.position);}
void SpaceReverbGraph::mouseUp(const juce::MouseEvent&){const auto keep=state;keep->end();}
void SpaceReverbGraph::mouseDoubleClick(const juce::MouseEvent& e){const auto keep=state;const auto scope=keep->frame();if(keep->retired||keep->closing()||!keep->slot||!e.mods.isLeftButtonDown()||!plotBounds().contains(e.position))return;for(std::size_t n=0;n<6;++n)if(keep->bandActive(n)&&nodePosition(n).getDistanceFrom(e.position)<14)return;keep->addBand(e.position);}
void SpaceReverbGraph::mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& wheel){const auto keep=state;if(!keep->retired&&keep->slot&&plotBounds().contains(e.position)&&wheel.deltaY!=0)keep->wheelQ(wheel.deltaY);}
bool SpaceReverbGraph::keyPressed(const juce::KeyPress& key){
    const auto keep=state;const auto scope=keep->frame();if(keep->retired||keep->closing()||!keep->slot)return false;const int code=key.getKeyCode();
    if(code=='D'||code=='d'){setMode(Mode::decay);return true;}if(code=='P'||code=='p'){setMode(Mode::postEq);return true;}
    if(code>='1'&&code<='6'){selectBand(std::size_t(code-'1'));return true;}
    if(code==juce::KeyPress::returnKey){keep->notifySelection();return true;}
    if(code==juce::KeyPress::escapeKey){keep->end();return true;}
    if(code==juce::KeyPress::insertKey){keep->addBand({});return true;}
    if(code==juce::KeyPress::deleteKey||code==juce::KeyPress::backspaceKey){keep->setField(0,0);return true;}
    if(code=='A'||code=='a'){keep->toggle(0);return true;}if(code=='B'||code=='b'){keep->toggle(1);return true;}
    if(code=='+'||code=='='||code=='-'){keep->wheelQ((code=='-'?-.1:.1)*(key.getModifiers().isShiftDown()?.2:1));return true;}
    if(code!=juce::KeyPress::leftKey&&code!=juce::KeyPress::rightKey&&code!=juce::KeyPress::upKey&&code!=juce::KeyPress::downKey)return false;
    auto at=nodePosition(keep->chosen());const float step=key.getModifiers().isShiftDown()?1:5;
    if(code==juce::KeyPress::leftKey)at.x-=step;if(code==juce::KeyPress::rightKey)at.x+=step;if(code==juce::KeyPress::upKey)at.y-=step;if(code==juce::KeyPress::downKey)at.y+=step;
    if(keep->beginNode()){keep->moveNode(at);keep->end();}return true;
}
void SpaceReverbGraph::focusLost(FocusChangeType){const auto keep=state;keep->end();}
void SpaceReverbGraph::visibilityChanged(){if(state&&!isShowing())suspend();}
std::unique_ptr<juce::AccessibilityHandler> SpaceReverbGraph::createAccessibilityHandler(){return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::group);}
}
