#include "EqOverview.h"
#include "InstrumentProcessor.h"
#include "DotMatrix.h"
#include "ProgrammerLook.h"
#include <algorithm>
#include <cmath>

namespace airack::studio
{
namespace
{
const juce::Colour mint(0xff91e5ce),blue(0xff73b5ff),paper(0xffc3d2d6),grid(0xff29383b);
using Levels=std::array<float,128>;
double frequency(std::size_t i){return 20*std::pow(1000.,double(i)/127);}
juce::String memberName(const companion::GroupMemberSummary& m){
    const auto role=groupTextView(m.intent.role);const auto name=juce::String::fromUTF8(role.data(),int(role.size()));
    return (name.isEmpty()?"Member":name)+" / "+juce::Uuid(m.intent.member.getRawData()).toString().substring(0,6);
}
void trace(juce::Graphics& g,juce::Rectangle<float> r,const Levels& db,juce::Colour ink,bool dashed=false,float low=0,float high=1){
    juce::Graphics::ScopedSaveState saved(g);g.reduceClipRegion(r.toNearestInt());juce::Path p;
    for(std::size_t i=0;i<db.size();++i){const auto x=r.getX()+r.getWidth()*(float(i)/127-low)/(high-low);const auto y=r.getBottom()-r.getHeight()*std::clamp((db[i]+120)/120,0.f,1.f);if(i==0)p.startNewSubPath(x,y);else p.lineTo(x,y);}
    g.setColour(ink);
    if(dashed){juce::Path d;const float dash[]{4,3};juce::PathStrokeType(1.1f).createDashedStroke(d,p,dash,2);g.fillPath(d);}
    else g.strokePath(p,juce::PathStrokeType(1.4f));
}
struct Keys final:juce::LookAndFeel_V4 {
    void drawButtonBackground(juce::Graphics& g,juce::Button& b,const juce::Colour&,bool over,bool down) override {
        const auto r=b.getLocalBounds().toFloat().reduced(.6f);g.setColour(b.getToggleState()?mint.withAlpha(.13f):juce::Colour(0xff172125));g.fillRect(r);
        if(over||down){g.setColour(mint.withAlpha(down?.17f:.07f));g.fillRect(r);}
        g.setColour(ProgrammerLook::ringed(b)?mint:grid.brighter(.15f));g.drawRect(r,1);
    }
    void drawButtonText(juce::Graphics& g,juce::TextButton& b,bool,bool) override {
        g.setFont(juce::FontOptions(13));g.setColour((b.getToggleState()?mint:paper).withAlpha(b.isEnabled()?1.f:.4f));g.drawFittedText(b.getButtonText(),b.getLocalBounds().reduced(4,1),juce::Justification::centred,1);
    }
};
struct SourceKey final:KeyFocus<juce::TextButton> {
    std::function<void(juce::Graphics&,juce::Rectangle<float>)> draw;
    std::function<void()> context,navigate;
    void paintButton(juce::Graphics& g,bool over,bool down) override {
        if(draw)draw(g,getLocalBounds().toFloat());
        if(over||down){g.setColour(paper.withAlpha(down?.1f:.04f));g.fillRect(getLocalBounds());}
        if(ringed()){g.setColour(mint);g.drawRect(getLocalBounds().reduced(1),1);}
    }
    void mouseDown(const juce::MouseEvent& e) override {if(e.mods.isPopupMenu()){if(context)context();return;}KeyFocus::mouseDown(e);}
    void mouseDoubleClick(const juce::MouseEvent&) override {if(navigate)navigate();}
    bool keyPressed(const juce::KeyPress& k) override {if(k.getKeyCode()==juce::KeyPress::F10Key&&k.getModifiers().isShiftDown()){if(context)context();return true;}return KeyFocus::keyPressed(k);}
};
}
std::bitset<128> potentialEqOverlap(const Levels& a,const Levels& b) noexcept {
    std::bitset<128> result;for(std::size_t i=0;i<128;++i)result.set(i,std::isfinite(a[i])&&std::isfinite(b[i])&&a[i]>-72&&b[i]>-72&&std::abs(a[i]-b[i])<=12);return result;
}
Levels sampleEqSpectrum(const FxSpectrum::Result& value,bool output) noexcept {
    Levels result;result.fill(-120);
    if(!value.valid||!std::isfinite(value.sampleRate)||value.sampleRate<=0)return result;
    const auto& bins=output?value.outputDb:value.inputDb;const auto halfStep=std::pow(1000.,.5/127);
    for(std::size_t i=0;i<128;++i){
        const auto centre=frequency(i),low=i==0?20.:centre/halfStep,high=std::min(value.sampleRate*.5,i==127?20000.:centre*halfStep);
        if(low>high)continue;
        // Peak-preserving logarithmic cells: point sampling would lose narrow
        // tones between the 128 centres. Every FFT bin is visited at most once.
        const auto first=std::max(1,int(std::ceil(low*FxSpectrum::fftSize/value.sampleRate)));
        const auto last=std::min(FxSpectrum::bins-1,int(std::floor(high*FxSpectrum::fftSize/value.sampleRate)));
        float level=FxSpectrum::dbAt(value,centre,output);
        for(int bin=first;bin<=last;++bin)if(std::isfinite(bins[std::size_t(bin)]))level=std::max(level,bins[std::size_t(bin)]);
        result[i]=std::isfinite(level)?std::clamp(level,-120.f,24.f):-120.f;
    }
    return result;
}
struct EqOverview::State
{
    EqOverview& view;InstrumentProcessor& processor;std::shared_ptr<FxAnalysisHub> hub;ProgrammerLook chassis;Keys keys;juce::Image rim;
    struct Local {bool present=false,seen=false;juce::String label;std::optional<FxAnalysisHub::Lease> lease;std::uint64_t generation=0,windows=0;double arrival=0;Levels input{},output{};};
    struct Peer {GroupUuid member,runtime;juce::String label;bool available=false,knownSlot=false;std::uint8_t slot=0;Levels output{};};
    std::array<Local,8> local;std::array<Peer,4> peers{};std::size_t peerCount=0;
    std::array<std::unique_ptr<SourceKey>,8> rows;std::array<std::unique_ptr<KeyFocus<juce::TextButton>>,8> references;
    std::array<std::unique_ptr<SourceKey>,4> peerRows;
    KeyFocus<juce::TextButton> group{"GROUP: OFF"},send{"SEND: EQ8"},back{"BACK"},add{"+"},input{"Local input"},output{"Output"};
    std::optional<std::uint8_t> reference,comparison;std::optional<GroupUuid> comparisonPeer;
    std::uint8_t sendSlot=7;std::uint64_t owner=0;bool retired=false;float low=0,high=1;
    State(EqOverview&,InstrumentProcessor&,const Model&,std::shared_ptr<FxAnalysisHub>);
    ~State(){for(auto* key:{&group,&send,&back,&add,&input,&output})key->setLookAndFeel(nullptr);for(auto& ref:references)ref->setLookAndFeel(nullptr);}
    bool available(std::uint8_t slot) const {if(slot>=8)return false;const auto& l=local[slot];const auto now=juce::Time::getMillisecondCounterHiRes();return l.present&&l.lease&&l.seen&&now>=l.arrival&&now-l.arrival<=2000;}
    bool peerLive(const Peer& p) const {return p.available&&processor.peerEqSpectrum(owner,p.runtime)!=nullptr;}
    const Levels* comparisonLevels() const {if(comparison&&available(*comparison))return &local[*comparison].output;if(comparisonPeer)for(const auto& p:peers)if(comparisonPeer==p.member&&peerLive(p))return &p.output;return nullptr;}
    void deactivate(){for(auto& l:local){if(l.lease)hub->release(*l.lease);l.lease.reset();l.seen=false;}if(owner)processor.closeEqSpectrumView(owner);owner=0;peers={};peerCount=0;comparisonPeer.reset();}
    void syncVisibility(){
        if(retired||!view.isShowing()){deactivate();return;}
        if(!owner)owner=processor.openEqSpectrumView();
        for(std::uint8_t i=0;i<8;++i){auto& l=local[i];if(l.present&&!l.lease){l.lease=hub->acquire(i);if(l.lease)if(const auto* r=hub->result(*l.lease)){l.generation=r->generation;l.windows=r->windows;}}
            else if(!l.present&&l.lease){hub->release(*l.lease);l.lease.reset();l.seen=false;}}
    }
    void syncKeys(){
        const auto sharing=processor.eqSpectrumView(owner);group.setButtonText(sharing.enabled?"GROUP: ON":"GROUP: OFF");group.setToggleState(sharing.enabled,juce::dontSendNotification);send.setButtonText("SEND: EQ"+juce::String(sendSlot+1));send.setEnabled(reference.has_value());add.setEnabled(sharing.enabled&&sharing.capable&&sharing.admitted);
        for(std::size_t i=0;i<8;++i){rows[i]->setVisible(local[i].present);references[i]->setVisible(local[i].present);references[i]->setToggleState(reference==i,juce::dontSendNotification);}
        for(std::size_t i=0;i<4;++i)peerRows[i]->setVisible(i<peerCount);
        const auto hint=view.potentialOverlap();view.setDescription("R selects reference; source rows select comparison. Shift F10 opens source actions. Escape returns to the rack. "+juce::String(hint?(hint->any()?"Potential overlap in visible frequencies; indication only.":"No potential overlap indicated in visible frequencies."):"Potential overlap unavailable."));
    }
    void paintRow(juce::Graphics&,juce::Rectangle<float>,const juce::String&,bool,const Levels&,const Levels&,int,bool);
    void sendMenu();void localMenu(std::uint8_t);void peerMenu(std::optional<std::size_t>);
};

EqOverview::State::State(EqOverview& v,InstrumentProcessor& p,const Model& m,std::shared_ptr<FxAnalysisHub> h):view(v),processor(p),hub(std::move(h)),chassis(m){
    const auto plate=m.plates.find("session");const auto display=m.displays.find("session");if(plate!=m.plates.end()&&display!=m.displays.end())rim=plate->second.getClippedImage(display->second.lattice.glass.toNearestInt().expanded(3));
    int order=1;for(auto* key:{&group,&send,&back,&add,&input,&output}){view.addAndMakeVisible(key);key->setLookAndFeel(&keys);key->setExplicitFocusOrder(order++);key->setComponentID("eq.overview."+(key==&group?juce::String("group"):key==&send?"send":key==&back?"back":key==&add?"add":key==&input?"input":"output"));key->setTitle(key->getButtonText());}
    add.setExplicitFocusOrder(27);input.setExplicitFocusOrder(40);output.setExplicitFocusOrder(41);
    input.setClickingTogglesState(true);output.setClickingTogglesState(true);output.setToggleState(true,juce::dontSendNotification);
    input.setTitle("Show local input spectrum (dashed)");output.setTitle("Show output spectra (solid)");input.onClick=output.onClick=[this]{view.repaint();};
    group.onClick=[this]{if(!retired)(void)view.setGroupEnabled(!processor.eqSpectrumView(owner).enabled);};send.onClick=[this]{sendMenu();};add.onClick=[this]{peerMenu({});};back.onClick=[this]{auto callback=view.onClose;if(callback)callback();};
    group.setTooltip("Explicitly share this EQ with your admitted group. Resume restored group consent in Group first.");send.setTooltip("One local EQ source, at most ten summaries per second.");add.setTitle("Select up to four admitted group sources");back.setTooltip("Return to the rack (Escape)");
    for(std::size_t i=0;i<8;++i){
        rows[i]=std::make_unique<SourceKey>();references[i]=std::make_unique<KeyFocus<juce::TextButton>>("R");auto& row=*rows[i];auto& ref=*references[i];view.addChildComponent(row);view.addChildComponent(ref);ref.setLookAndFeel(&keys);
        row.setComponentID("eq.overview.local."+juce::String(int(i)));ref.setComponentID("eq.overview.reference."+juce::String(int(i)));row.setExplicitFocusOrder(10+int(i)*2);ref.setExplicitFocusOrder(9+int(i)*2);
        ref.onClick=[this,i]{(void)view.setReferenceSlot(std::uint8_t(i));};row.onClick=[this,i]{(void)view.setComparisonSlot(std::uint8_t(i));};row.navigate=[this,i]{auto callback=view.onNavigate;if(callback)callback(std::uint8_t(i));};row.context=[this,i]{localMenu(std::uint8_t(i));};
        row.draw=[this,i](juce::Graphics& g,juce::Rectangle<float> r){paintRow(g,r,local[i].label,available(std::uint8_t(i)),local[i].input,local[i].output,reference==i?1:comparison==i&&!comparisonPeer?2:0,false);};
    }
    for(std::size_t i=0;i<4;++i){auto& row=*(peerRows[i]=std::make_unique<SourceKey>());view.addChildComponent(row);row.setComponentID("eq.overview.peer."+juce::String(int(i)));row.setExplicitFocusOrder(30+int(i));row.onClick=[this,i]{(void)view.setComparisonPeer(peers[i].member);};row.context=[this,i]{peerMenu(i);};row.draw=[this,i](juce::Graphics& g,juce::Rectangle<float> r){const auto& p=peers[i];paintRow(g,r,p.label,peerLive(p),{},p.output,comparisonPeer==p.member?2:0,true);};}
}
void EqOverview::State::paintRow(juce::Graphics& g,juce::Rectangle<float> r,const juce::String& label,bool live,const Levels& pre,const Levels& post,int selected,bool peer){
    g.setColour(selected?(selected==1?mint:blue).withAlpha(.15f):juce::Colour(0xff101b1e));g.fillRect(r);g.setColour(grid);g.drawRect(r,1);g.setColour(paper);g.setFont(juce::FontOptions(14));g.drawFittedText(label,r.withTrimmedRight(106).reduced(7,1).toNearestInt(),juce::Justification::centredLeft,1,1.f);
    const auto mini=juce::Rectangle<float>(r.getRight()-101,r.getY()+4,78,r.getHeight()-8);
    if(live){if(!peer)trace(g,mini,pre,mint.withAlpha(.6f),true);trace(g,mini,post,peer?blue:mint);}
    else{g.setColour(paper.withAlpha(.45f));g.setFont(juce::FontOptions(11));g.drawText("Unavailable",mini,juce::Justification::centred);}
    g.setColour(live?(peer?blue:mint):paper.withAlpha(.25f));g.fillEllipse(r.getRight()-14,r.getCentreY()-3,6,6);
}
void EqOverview::State::sendMenu(){
    if(retired)return;
    juce::PopupMenu menu;for(std::uint8_t i=0;i<8;++i)if(local[i].present)menu.addItem(i+1,local[i].label,true,i==sendSlot);
    juce::Component::SafePointer<EqOverview> safe(&view);menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&send),[safe](int item){if(safe&&item>0)(void)safe->setSendSlot(std::uint8_t(item-1));});
}
void EqOverview::State::localMenu(std::uint8_t slot){
    if(retired)return;
    juce::PopupMenu menu;menu.addItem(1,"Use as reference");menu.addItem(2,"Use as comparison");menu.addItem(3,"Go to this rack");juce::Component::SafePointer<EqOverview> safe(&view);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(rows[slot].get()),[safe,slot](int item){if(!safe)return;if(item==1)(void)safe->setReferenceSlot(slot);else if(item==2)(void)safe->setComparisonSlot(slot);else if(item==3){auto callback=safe->onNavigate;if(callback)callback(slot);}});
}
void EqOverview::State::peerMenu(std::optional<std::size_t> row){
    if(retired||!owner)return;const auto sharing=processor.eqSpectrumView(owner);if(!sharing.enabled||!sharing.admitted||!sharing.capable)return;
    const auto workflow=processor.groupWorkflowView();std::vector<companion::GroupMemberSummary> choices;
    for(const auto& m:workflow.members)if(workflow.link.intent&&m.intent.group==workflow.link.intent->group&&m.runtime!=workflow.link.runtime&&std::find(sharing.peers.begin(),sharing.peers.begin()+sharing.peerCount,m.intent.member)==sharing.peers.begin()+sharing.peerCount)choices.push_back(m);
    juce::PopupMenu menu;for(std::size_t i=0;i<choices.size();++i)menu.addItem(int(i+1),memberName(choices[i]),row.has_value()||sharing.peerCount<4);if(choices.empty())menu.addItem(99,"No additional admitted sources",false);if(row){menu.addSeparator();menu.addItem(100,"Remove source");}
    juce::Component::SafePointer<EqOverview> safe(&view);const auto capturedOwner=owner;const auto captured=sharing;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(row?static_cast<juce::Component*>(peerRows[*row].get()):&add),[safe,choices=std::move(choices),capturedOwner,captured,row](int item){
        if(!safe||safe->state->retired||safe->state->owner!=capturedOwner||safe->state->processor.eqSpectrumView(capturedOwner)!=captured)return;
        auto selected=captured.peers;auto count=captured.peerCount;
        if(item==100&&row&&*row<count){std::move(selected.begin()+*row+1,selected.begin()+count,selected.begin()+*row);--count;}
        else if(item>0&&std::size_t(item)<=choices.size()){if(row&&*row<count)selected[*row]=choices[std::size_t(item-1)].intent.member;else if(count<4)selected[count++]=choices[std::size_t(item-1)].intent.member;else return;}
        else return;(void)safe->setPeerSelection({selected.data(),count});
    });
}
EqOverview::EqOverview(InstrumentProcessor& p,const Model& m,std::shared_ptr<FxAnalysisHub> hub):state(std::make_unique<State>(*this,p,m,std::move(hub))){setComponentID("eq.overview");setTitle("EQ spectrum overview");setDescription("Local reference and comparison spectra. R selects reference; source rows select comparison. Shift F10 opens source actions. Potential overlap is an indication only. Escape returns to the rack.");setWantsKeyboardFocus(true);setOpaque(true);}
EqOverview::~EqOverview(){retire();}
void EqOverview::retire(){
    auto& s=*state;if(s.retired)return;s.retired=true;s.deactivate();onClose=nullptr;onNavigate=nullptr;
    for(auto* key:{&s.group,&s.send,&s.back,&s.add,&s.input,&s.output})key->onClick=nullptr;
    for(std::size_t i=0;i<8;++i){s.references[i]->onClick=nullptr;s.rows[i]->onClick=s.rows[i]->context=s.rows[i]->navigate=nullptr;}
    for(auto& row:s.peerRows)row->onClick=row->context=row->navigate=nullptr;
}
void EqOverview::refresh(const SoundDocument& d){
    auto& s=*state;if(s.retired)return;
    for(std::uint8_t i=0;i<8;++i){auto& l=s.local[i];l.present=d.layout.effects[i].use==SlotUse::present&&d.layout.effects[i].kind==EffectKind::dynamicEq;l.label="EQ "+juce::String(i+1);
        if(d.personalities)for(const auto& rack:d.personalities->instances)for(const auto& unit:rack.design.units)if(unit.module==NativeModule::dynamicEq)if(const auto* b=rackUnitBinding(rack,unit.id);b&&b->slot==i)l.label+=" / "+rack.design.name;
        s.rows[i]->setTitle(l.label+" comparison; double-click to navigate");s.rows[i]->setTooltip(l.label);s.references[i]->setTitle(l.label+" reference");}
    if(!s.reference||!s.local[*s.reference].present){s.reference.reset();for(std::uint8_t i=0;i<8;++i)if(s.local[i].present){s.reference=i;break;}}
    if(!s.local[s.sendSlot].present&&s.reference){s.deactivate();s.sendSlot=*s.reference;}
    if(s.comparison&&!s.local[*s.comparison].present)s.comparison.reset();
    s.syncVisibility();s.syncKeys();resized();repaint();
}
void EqOverview::updateMonitoring(){
    auto& s=*state;if(s.retired)return;s.syncVisibility();if(!s.owner)return;const auto now=juce::Time::getMillisecondCounterHiRes();
    for(std::uint8_t i=0;i<8;++i){auto& l=s.local[i];if(l.lease)if(const auto* r=s.hub->result(*l.lease);r&&r->valid&&(r->generation!=l.generation||r->windows!=l.windows)){
        l.generation=r->generation;l.windows=r->windows;l.arrival=now;l.seen=true;l.input=sampleEqSpectrum(*r,false);l.output=sampleEqSpectrum(*r,true);
        if(i==s.sendSlot){std::array<std::int16_t,128> levels;for(std::size_t n=0;n<128;++n)levels[n]=std::int16_t(std::lround(l.output[n]*100));(void)s.processor.captureEqSpectrum(s.owner,i,r->generation,r->windows,r->sampleRate,levels);}
    }else if(!r||!r->valid)l.seen=false;}
    const auto sharing=s.processor.eqSpectrumView(s.owner);const auto workflow=s.processor.groupWorkflowView();s.peerCount=sharing.peerCount;
    for(std::size_t i=0;i<4;++i){auto& peer=s.peers[i];if(i>=s.peerCount){peer={};continue;}if(peer.member!=sharing.peers[i])peer={};peer.member=sharing.peers[i];peer.available=false;if(peer.label.isEmpty())peer.label="Member / unavailable";
        const auto found=std::find_if(workflow.members.begin(),workflow.members.end(),[&](const auto& m){return m.intent.member==peer.member;});
        if(found!=workflow.members.end()){if(peer.runtime!=found->runtime)peer.knownSlot=false;peer.runtime=found->runtime;peer.label=memberName(*found);if(const auto* data=s.processor.peerEqSpectrum(s.owner,peer.runtime)){peer.available=peer.knownSlot=true;peer.slot=data->slot;for(std::size_t n=0;n<128;++n)peer.output[n]=data->postDb100[n]/100.f;}if(peer.knownSlot)peer.label+=" / EQ"+juce::String(peer.slot+1);}
        s.peerRows[i]->setTitle(peer.label+(peer.available?" comparison":" unavailable"));s.peerRows[i]->setTooltip(peer.label+"; Shift F10 to change or remove source");
    }
    s.syncKeys();resized();repaint();
}
bool EqOverview::setReferenceSlot(std::uint8_t slot){auto& s=*state;if(s.retired||slot>=8||!s.local[slot].present)return false;s.reference=slot;s.syncKeys();repaint();return true;}
bool EqOverview::setComparisonSlot(std::uint8_t slot){auto& s=*state;if(s.retired||slot>=8||!s.local[slot].present)return false;s.comparison=slot;s.comparisonPeer.reset();s.syncKeys();repaint();return true;}
bool EqOverview::setComparisonPeer(GroupUuid member){auto& s=*state;if(s.retired)return false;const auto sharing=s.processor.eqSpectrumView(s.owner);if(std::find(sharing.peers.begin(),sharing.peers.begin()+sharing.peerCount,member)==sharing.peers.begin()+sharing.peerCount)return false;s.comparisonPeer=member;s.comparison.reset();s.syncKeys();repaint();return true;}
bool EqOverview::setGroupEnabled(bool enabled){auto& s=*state;if(s.retired||!s.owner||!s.reference)return false;const auto link=s.processor.groupLinkView();if(enabled&&link.phase!=GroupLinkPhase::authorized&&link.phase!=GroupLinkPhase::admitted)return false;const auto sharing=s.processor.eqSpectrumView(s.owner);if(!s.processor.configureEqSpectrumView(s.owner,enabled,s.sendSlot,{sharing.peers.data(),sharing.peerCount}))return false;if(enabled)(void)s.processor.browseGroups();s.syncKeys();repaint();return true;}
bool EqOverview::setPeerSelection(std::span<const GroupUuid> peers){auto& s=*state;if(s.retired||!s.owner)return false;const auto sharing=s.processor.eqSpectrumView(s.owner);if(!s.processor.configureEqSpectrumView(s.owner,sharing.enabled,s.sendSlot,peers))return false;if(s.comparisonPeer&&std::find(peers.begin(),peers.end(),*s.comparisonPeer)==peers.end())s.comparisonPeer.reset();updateMonitoring();return true;}
bool EqOverview::setSendSlot(std::uint8_t slot){auto& s=*state;if(s.retired||!s.owner||slot>=8||!s.local[slot].present)return false;const auto sharing=s.processor.eqSpectrumView(s.owner);if(!s.processor.configureEqSpectrumView(s.owner,sharing.enabled,slot,{sharing.peers.data(),sharing.peerCount}))return false;s.sendSlot=slot;s.syncKeys();return true;}
std::optional<std::uint8_t> EqOverview::referenceSlot() const{return state->reference;}
std::size_t EqOverview::localCount() const noexcept{return std::count_if(state->local.begin(),state->local.end(),[](const auto& l){return l.present;});}
std::size_t EqOverview::peerCount() const noexcept{return state->peerCount;}
bool EqOverview::localAvailable(std::uint8_t slot) const{return !state->retired&&state->available(slot);}
std::optional<std::bitset<128>> EqOverview::potentialOverlap() const{
    const auto& s=*state;if(s.retired||!s.reference||!s.available(*s.reference)||!s.output.getToggleState())return {};const auto* compared=s.comparisonLevels();if(!compared)return {};
    auto bins=potentialEqOverlap(s.local[*s.reference].output,*compared);
    // The hint and contributing shading describe the plot's visible range.
    for(std::size_t i=0;i<128;++i){const auto n=float(i)/127;if(n<s.low||n>s.high)bins.reset(i);}
    return bins;
}
juce::String EqOverview::statusText() const{const auto& s=*state;if(s.retired)return "Overview closed";if(!s.owner)return "Group off / local analysis";const auto sharing=s.processor.eqSpectrumView(s.owner);if(!sharing.enabled)return "Group off / local analysis";if(!sharing.capable)return "Group spectrum unavailable / local analysis";if(!sharing.admitted)return "Group not admitted / local analysis";return "Group connected / "+juce::String(sharing.peerCount)+" selected";}
juce::Rectangle<float> EqOverview::plotBounds() const{return {404,125,572,248};}
void EqOverview::paint(juce::Graphics& g){
    auto& s=*state;g.fillAll(juce::Colour(0xff111416));s.chassis.drawFace(g,{26,0,1028,526});
    for(int y=14;y<526;y+=28){g.setColour(juce::Colour(0xff050708));g.fillRoundedRectangle(5.f,float(y),14,17,1);g.fillRoundedRectangle(1061.f,float(y),14,17,1);}
    const juce::Rectangle<int> frame(50,27,980,468);g.setColour(juce::Colour(0xff0c191c));g.fillRoundedRectangle(frame.toFloat().reduced(3),3);drawDisplayRim(g,s.rim,frame);
    const auto text=[&](const juce::String& value,juce::Rectangle<int> r,float font=13.f,juce::Colour ink=paper,int align=juce::Justification::centredLeft){g.setColour(ink);g.setFont(juce::FontOptions(font));g.drawFittedText(value,r,align,1,1.f);};
    text("EQ OVERVIEW",{64,41,272,32},24,mint);text(juce::String(int(localCount()))+" local / "+juce::String(int(s.peerCount))+" peers",{398,41,260,32},13);
    g.setColour(grid);g.drawHorizontalLine(83,64,1016);text("REF   THIS INSTANCE",{64,89,288,20});text("ADMITTED GROUP  "+juce::String(int(s.peerCount))+" / 4",{64,307,252,24});
    if(!localCount())text("No native EQ slots",{64,119,288,30},13);
    if(!s.peerCount){text("Choose sources with + after opting in",{72,344,272,32},12,paper.withAlpha(.55f));text("Each peer chooses its own SEND slot",{72,380,272,28},11,paper.withAlpha(.45f));}
    const State::Local* reference=s.reference?&s.local[*s.reference]:nullptr;const Levels* compared=nullptr;juce::String comparisonName="Choose a comparison";
    if(s.comparison)comparisonName=s.local[*s.comparison].label;
    if(s.comparisonPeer)for(const auto& peer:s.peers)if(s.comparisonPeer==peer.member)comparisonName=peer.label;
    compared=s.comparisonLevels();
    text("REFERENCE: "+(reference?reference->label:"None"),{372,89,312,20},13,mint);text("COMPARE: "+comparisonName,{694,89,322,20},13,blue);
    const auto r=plotBounds();text("dBFS",{404,111,36,12},11);
    for(int db:{0,-30,-60,-90,-120}){const float y=r.getY()-r.getHeight()*float(db)/120;g.setColour(grid);g.drawLine(r.getX(),y,r.getRight(),y,1);text(juce::String(db),{366,int(y)-8,29,16},12,paper,juce::Justification::centredRight);}
    const auto frequencyTick=[&](double hz,float n){const auto x=r.getX()+r.getWidth()*(n-s.low)/(s.high-s.low);g.setColour(grid);g.drawVerticalLine(int(x),r.getY(),r.getBottom());text(hz>=1000?juce::String(hz/1000,s.high-s.low<.999f?2:0)+"k":juce::String(int(std::round(hz))),{int(x)-25,377,50,20},11,paper,juce::Justification::centred);};
    if(s.high-s.low>=.999f)for(double hz:{20.,50.,100.,200.,500.,1000.,2000.,5000.,10000.,20000.})frequencyTick(hz,float(std::log(hz/20)/std::log(1000.)));
    else for(int tick=0;tick<=5;++tick){const float n=s.low+(s.high-s.low)*float(tick)/5;frequencyTick(20*std::pow(1000.,n),n);}
    const auto hint=potentialOverlap();const bool overlap=hint&&hint->any(),referenceLive=s.reference&&s.available(*s.reference);
    if(overlap){
        juce::Graphics::ScopedSaveState saved(g);g.reduceClipRegion(r.toNearestInt());g.setColour(juce::Colour(0xffdfa65a).withAlpha(.09f));
        for(std::size_t i=0;i<128;++i)if((*hint)[i]){const float x=r.getX()+r.getWidth()*(float(i)/127-s.low)/(s.high-s.low);g.fillRect(x,r.getY(),std::max(1.f,r.getWidth()/127/(s.high-s.low)),r.getHeight());}
    }
    if(referenceLive){if(s.input.getToggleState())trace(g,r,reference->input,mint.withAlpha(.65f),true,s.low,s.high);if(s.output.getToggleState())trace(g,r,reference->output,mint,false,s.low,s.high);}
    else text("Reference unavailable - play audio to capture a fresh spectrum",{420,225,540,36},13,paper.withAlpha(.6f),juce::Justification::centred);
    if(compared&&s.output.getToggleState())trace(g,r,*compared,blue,false,s.low,s.high);
    else if(s.comparison||s.comparisonPeer)text("Comparison unavailable",{420,335,540,24},12,blue.withAlpha(.7f),juce::Justification::centred);
    g.setColour(mint);g.drawLine(632,419,662,419,1.5f);text("Reference",{672,408,110,22});g.setColour(blue);g.drawLine(826,419,856,419,1.5f);text("Comparison",{866,408,150,22});
    text("Wheel: zoom  /  Double-click: reset",{384,436,632,20},11,paper.withAlpha(.55f));g.setColour(grid);g.drawHorizontalLine(455,64,1016);
    text(statusText(),{64,459,440,22},12);text(hint?(overlap?"Potential overlap - indication only":s.high-s.low<.999f?"No potential overlap in view":"No potential overlap indicated"):"Potential overlap unavailable",{536,459,480,22},12,overlap?juce::Colour(0xffdfa65a):paper.withAlpha(.6f),juce::Justification::centredRight);
}
void EqOverview::resized(){
    auto& s=*state;s.group.setBounds(688,43,136,28);s.send.setBounds(832,43,120,28);s.back.setBounds(960,43,56,28);s.add.setBounds(328,311,24,20);s.input.setBounds(384,409,112,20);s.output.setBounds(504,409,96,20);
    int row=0;for(std::size_t i=0;i<8;++i)if(s.local[i].present){s.references[i]->setBounds(64,111+24*row,24,24);s.rows[i]->setBounds(88,111+24*row,264,24);++row;}
    for(int i=0;i<4;++i)s.peerRows[std::size_t(i)]->setBounds(64,335+i*24,288,24);
}
void EqOverview::visibilityChanged(){state->syncVisibility();}
bool EqOverview::keyPressed(const juce::KeyPress& k){
    if(state->retired)return false;if(k.getKeyCode()==juce::KeyPress::escapeKey){auto callback=onClose;if(callback)callback();return true;}
    if(k==juce::KeyPress('0')){state->low=0;state->high=1;repaint();return true;}
    if(k.getKeyCode()==juce::KeyPress::upKey||k.getKeyCode()==juce::KeyPress::downKey){const int direction=k.getKeyCode()==juce::KeyPress::upKey?-1:1;for(int n=1;n<=8;++n){const auto slot=std::uint8_t((int(state->reference.value_or(0))+direction*n+16)%8);if(state->local[slot].present)return setReferenceSlot(slot);}return true;}return false;
}
void EqOverview::mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& wheel){if(state->retired||!plotBounds().contains(e.position))return;auto& s=*state;const auto anchor=std::clamp((e.position.x-plotBounds().getX())/plotBounds().getWidth(),0.f,1.f);const auto old=s.high-s.low,span=std::clamp(old*std::exp(-wheel.deltaY*2),.05f,1.f);s.low=std::clamp(s.low+old*anchor-span*anchor,0.f,1-span);s.high=s.low+span;repaint();}
void EqOverview::mouseDoubleClick(const juce::MouseEvent& e){if(!state->retired&&plotBounds().contains(e.position)){state->low=0;state->high=1;repaint();}}
std::unique_ptr<juce::AccessibilityHandler> EqOverview::createAccessibilityHandler(){return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::group);}
}
