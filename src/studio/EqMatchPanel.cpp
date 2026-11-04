#include "EqMatchPanel.h"
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
const juce::Colour mint(0xff91e5ce),amber(0xffdfa65a),paper(0xffc3d2d6),grid(0xff29383b),blue(0xff73b5ff);
double frequency(std::size_t i){return 20*std::pow(1000.,double(i)/128);}
struct Keys final:juce::LookAndFeel_V4 {
    void drawButtonBackground(juce::Graphics& g,juce::Button& b,const juce::Colour&,bool over,bool down) override {
        const auto r=b.getLocalBounds().toFloat().reduced(.6f);g.setColour(b.getToggleState()?mint.withAlpha(.13f):juce::Colour(0xff172125));g.fillRect(r);
        if(over||down){g.setColour(mint.withAlpha(down?.17f:.07f));g.fillRect(r);}g.setColour(ProgrammerLook::ringed(b)?mint:grid.brighter(.2f));g.drawRect(r,1);
    }
    void drawButtonText(juce::Graphics& g,juce::TextButton& b,bool,bool) override {
        g.setFont(juce::FontOptions(12));g.setColour((b.getToggleState()?mint:paper).withAlpha(b.isEnabled()?1.f:.4f));g.drawFittedText(b.getButtonText(),b.getLocalBounds().reduced(5,1),juce::Justification::centred,1,1.f);
    }
    void drawLinearSlider(juce::Graphics& g,int x,int y,int w,int h,float position,float,float,juce::Slider::SliderStyle,juce::Slider&) override {
        const auto middle=float(y)+float(h)/2;g.setColour(grid);g.fillRect(float(x),middle-1,float(w),2.f);g.setColour(mint);g.fillRect(float(x),middle-1,position-x,2.f);g.setColour(paper);g.fillRect(position-4,middle-5,8.f,10.f);
    }
};
std::optional<EqReference> localSnapshot(const FxSpectrum::Result& result,bool output)
{
    if(!result.valid||!std::isfinite(result.sampleRate)||result.sampleRate<=0)return {};
    EqReference r;r.sampleRate=result.sampleRate;r.label=output?"Local output":"Local input";r.captureId=juce::Uuid().toString();r.spectrum.db.fill(-120);
    const auto& db=output?result.outputDb:result.inputDb;const auto half=std::pow(1000.,.5/128);
    for(std::size_t i=0;i<129;++i){const auto hz=frequency(i);if(hz>result.sampleRate*.5)continue;
        const auto low=std::max(0,int(std::ceil(hz/half*FxSpectrum::fftSize/result.sampleRate))),high=std::min(FxSpectrum::bins-1,int(std::floor(hz*half*FxSpectrum::fftSize/result.sampleRate)));
        float value=-120;
        if(low<=high){for(int k=low;k<=high;++k){if(!std::isfinite(db[std::size_t(k)]))return {};value=std::max(value,db[std::size_t(k)]);}}
        else{const auto position=hz*FxSpectrum::fftSize/result.sampleRate;const auto lo=std::size_t(std::floor(position)),hi=std::min(lo+1,db.size()-1);
            if(!std::isfinite(db[lo])||!std::isfinite(db[hi]))return {};value=float(10*std::log10(std::max(1e-12,std::lerp(std::pow(10.,db[lo]/10),std::pow(10.,db[hi]/10),position-lo))));}
        r.spectrum.db[i]=std::clamp(value,-120.f,120.f);r.valid.set(i);
    }return r;
}
std::optional<EqReference> peerSnapshot(const companion::GroupSpectrum& data,const juce::String& label)
{
    if(!companion::validateGroupSpectrum(data))return {};
    EqReference r;r.sampleRate=data.sampleRate;r.label=label.substring(0,64);r.captureId=juce::Uuid(data.runtime.getRawData()).toString()+"/"+juce::String(data.loadRevision)+"/"+juce::String(data.generation)+"/"+juce::String(data.sequence);r.spectrum.db.fill(-120);
    // These are the admitted peer's coarse128 log cells, not a new high-resolution FFT.
    for(std::size_t i=0;i<129;++i){const auto position=double(i)*127/128;const auto lo=std::size_t(std::floor(position)),hi=std::min(lo+1,std::size_t(127));
        if(frequency(i)>data.sampleRate*.5||20*std::pow(1000.,double(hi)/127)>data.sampleRate*.5)continue;
        r.spectrum.db[i]=float(std::lerp(double(data.postDb100[lo]),double(data.postDb100[hi]),position-lo)/100);r.valid.set(i);
    }return r;
}
}
struct EqMatchPanel::State : std::enable_shared_from_this<State>
{
    EqMatchPanel& view;InstrumentProcessor& processor;std::shared_ptr<FxAnalysisHub> hub;ProgrammerLook chassis;Keys keys;juce::Image rim;
    juce::Component::SafePointer<DynamicEqGraph> graph;
    SoundDocument sound;std::uint64_t revision=0,reset=0;std::optional<std::uint8_t> slot;bool retired=false,sourceOutput=false,previewBypassed=false;
    std::optional<EqReference> source,reference;
    juce::String message="Learn this rack, then choose a reference";
    KeyFocus<juce::TextButton> back{"BACK"},spectra{"Spectra"},correction{"Correction"},sourceKey{"Input"},referenceKey{"Choose reference"},learnKey{"LEARN"},load{"LOAD..."},cancel{"CANCEL"},previewKey{"PREVIEW"},applyKey{"APPLY"};
    KeyFocus<juce::Slider> amount;
    std::unique_ptr<juce::FileChooser> chooser;
    struct Capture {
        FxAnalysisHub::Lease lease;bool reference=false,output=false;std::uint8_t slot=0;
        std::uint64_t generation=0,continuity=0,windows=0,start=0,end=0,dropped=0,clipped=0,producer=0;
        double rate=0,seconds=0,activity=0;std::size_t samples=0;
        std::array<double,129> energy{};std::bitset<129> valid;
    };
    std::optional<Capture> capture;double seconds=0,sourceSeconds=0;
    struct FileJob final:juce::ThreadPoolJob {
        explicit FileJob(juce::File p):ThreadPoolJob("EQ reference analysis"),file(std::move(p)){}
        juce::File file;std::atomic_bool cancel{false},done{false};std::optional<EqReference> result;
        JobStatus runJob() override {
            try{
                const auto size=file.getSize();
                if(!cancel.load()&&size>0&&size<=16384){juce::MemoryBlock bytes;if(file.loadFileAsData(bytes)&&bytes.getSize()<=16384&&juce::CharPointer_UTF8::isValidString(static_cast<const char*>(bytes.getData()),int(bytes.getSize()))){
                    EqReference decoded;if(decodeEqReference(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize())),decoded))result=std::move(decoded);}}
                if(!result&&!cancel.load())result=analyzeEqReferenceWav(file,cancel);
            }catch(...){result.reset();}
            done.store(true,std::memory_order_release);return jobHasFinished;
        }
    };
    std::unique_ptr<juce::ThreadPool> pool;std::unique_ptr<FileJob> job;
    std::uint64_t peerOwner=0;std::optional<GroupUuid> peer;std::optional<companion::GroupSpectrum> capturedPeer;
    std::optional<FxAnalysisHub::Lease> peerLease;
    State(EqMatchPanel&,InstrumentProcessor&,const Model&,std::shared_ptr<FxAnalysisHub>);
    ~State(){stopFile();}
    void stopFile(){if(job){job->cancel.store(true);if(pool)(void)pool->removeJob(job.get(),true,-1);job.reset();}}
    void stopCapture(){if(capture)hub->release(capture->lease);capture.reset();}
    void closePeer(){if(peerOwner)processor.closeEqSpectrumView(peerOwner);if(peerLease)hub->release(*peerLease);peerLease.reset();peerOwner=0;if(peer)reference.reset();peer.reset();capturedPeer.reset();}
    void invalidate(){if(graph)graph->cancelMatch();}
    const companion::GroupSpectrum* freshPeer() const {
        if(!peer||!peerOwner)return nullptr;const auto sharing=processor.eqSpectrumView(peerOwner);if(!sharing.enabled||!sharing.admitted||!sharing.capable)return nullptr;
        const auto workflow=processor.groupWorkflowView();const auto found=std::find_if(workflow.members.begin(),workflow.members.end(),[&](const auto& m){return m.intent.member==*peer;});
        return found==workflow.members.end()?nullptr:processor.peerEqSpectrum(peerOwner,found->runtime);
    }
    bool peerCurrent() const {
        if(!peer)return true;const auto* fresh=freshPeer();return fresh&&capturedPeer&&fresh->runtime==capturedPeer->runtime&&fresh->loadRevision==capturedPeer->loadRevision&&fresh->generation==capturedPeer->generation&&fresh->slot==capturedPeer->slot;
    }
    bool capturePeer(){
        const auto* data=freshPeer();if(!data)return false;const auto label="Peer "+juce::Uuid(peer->getRawData()).toString().substring(0,6)+" / EQ"+juce::String(data->slot+1)+" output";
        auto ref=peerSnapshot(*data,label);if(!ref)return false;capturedPeer=*data;reference=std::move(ref);return true;
    }
    void sync(){
        if(retired)return;const auto fit=graph?graph->matchPreview():DynamicEqGraph::MatchPreview{};
        const bool ready=source&&(peer?freshPeer()!=nullptr:reference.has_value())&&!capture&&!job&&fit.available>0;
        previewKey.setEnabled(ready);applyKey.setEnabled(ready&&peerCurrent()&&fit.canApply);applyKey.setToggleState(ready&&peerCurrent()&&fit.canApply,juce::dontSendNotification);
        sourceKey.setButtonText("EQ "+juce::String(slot?int(*slot+1):0)+" / "+(sourceOutput?"Output":"Input")+"  v");
        learnKey.setButtonText(capture?"STOP":"LEARN");learnKey.setEnabled(bool(graph)&&slot.has_value());
        referenceKey.setButtonText((reference?reference->label:peer?"Peer / waiting for fresh audio":"Choose reference")+"  v");referenceKey.setTooltip(reference?reference->label:"Choose local learning, a WAV or saved spectrum, or an admitted group source");
        view.setDescription("EQ Match. Explicitly learn input or output and choose a reference. Preview changes no sound. Apply adds unlocked unused Stereo bands with one undo. Escape returns to the rack. "+message);
        view.repaint();
    }
    void sourceMenu();void referenceMenu();void chooseFile(bool save);
};
EqMatchPanel::State::State(EqMatchPanel& v,InstrumentProcessor& p,const Model& m,std::shared_ptr<FxAnalysisHub> h):view(v),processor(p),hub(std::move(h)),chassis(m)
{
    const auto plate=m.plates.find("session");const auto display=m.displays.find("session");if(plate!=m.plates.end()&&display!=m.displays.end())rim=plate->second.getClippedImage(display->second.lattice.glass.toNearestInt().expanded(3));
    int order=1;for(auto* key:{&back,&spectra,&correction,&sourceKey,&learnKey,&referenceKey,&load,&cancel,&previewKey,&applyKey}){
        view.addAndMakeVisible(key);key->setLookAndFeel(&keys);key->setExplicitFocusOrder(order++);key->setTitle(key->getButtonText());}
    back.setComponentID("eq.match.back");spectra.setComponentID("eq.match.spectra");correction.setComponentID("eq.match.correction");sourceKey.setComponentID("eq.match.source");referenceKey.setComponentID("eq.match.reference");learnKey.setComponentID("eq.match.learn");load.setComponentID("eq.match.load");cancel.setComponentID("eq.match.cancel");previewKey.setComponentID("eq.match.preview");applyKey.setComponentID("eq.match.apply");
    correction.setToggleState(true,juce::dontSendNotification);correction.onClick=[this]{correction.setToggleState(true,juce::dontSendNotification);spectra.setToggleState(false,juce::dontSendNotification);view.repaint();};spectra.onClick=[this]{spectra.setToggleState(true,juce::dontSendNotification);correction.setToggleState(false,juce::dontSendNotification);view.repaint();};
    back.onClick=[this]{const auto keep=shared_from_this();auto callback=view.onClose;if(callback)callback();};
    sourceKey.onClick=[this]{sourceMenu();};referenceKey.onClick=[this]{referenceMenu();};load.onClick=[this]{chooseFile(false);};
    sourceKey.setTooltip("Input matching includes the retained static EQ. Output matches the captured signal; bypassed racks model their retained EQ without enabling it.");
    learnKey.onClick=[this]{if(capture)(void)view.stopLearning();else (void)view.learn(false,sourceOutput);};
    cancel.onClick=[this]{view.cancelPreview();};previewKey.onClick=[this]{(void)view.preview(float(amount.getValue()/100));};applyKey.onClick=[this]{(void)view.apply();};
    view.addAndMakeVisible(amount);amount.setLookAndFeel(&keys);amount.setComponentID("eq.match.amount");amount.setTitle("Match amount, percent");amount.setExplicitFocusOrder(8);amount.setRange(0,100,.1);amount.setValue(100,juce::dontSendNotification);amount.setSliderStyle(juce::Slider::LinearHorizontal);amount.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
    amount.onValueChange=[this]{const bool active=graph&&graph->matchPreview().active;invalidate();if(active&&!capture&&!job)(void)view.preview(float(amount.getValue()/100));else sync();};
}
void EqMatchPanel::State::sourceMenu()
{
    juce::PopupMenu menu;menu.addItem(1,"Input",true,!sourceOutput);menu.addItem(2,"Output",true,sourceOutput);juce::Component::SafePointer<EqMatchPanel> safe(&view);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sourceKey),[safe](int item){if(!safe||safe->state->retired||item<1||item>2)return;const auto keep=safe->state;keep->stopCapture();keep->stopFile();keep->invalidate();keep->sourceOutput=item==2;keep->source.reset();keep->seconds=0;keep->message="Learn the selected tap";keep->sync();});
}
void EqMatchPanel::State::referenceMenu()
{
    juce::PopupMenu menu;menu.addItem(1,"Load WAV or reference spectrum...");menu.addItem(2,"Export reference spectrum...",reference.has_value());menu.addSeparator();
    for(std::uint8_t i=0;i<8;++i)if(sound.layout.effects[i].use==SlotUse::present&&sound.layout.effects[i].kind==EffectKind::dynamicEq){menu.addItem(10+2*i,"Learn EQ "+juce::String(i+1)+" input");menu.addItem(11+2*i,"Learn EQ "+juce::String(i+1)+" output");}
    const auto workflow=processor.groupWorkflowView();std::vector<GroupUuid> choices;juce::PopupMenu group;
    for(const auto& m:workflow.members)if(workflow.link.intent&&m.intent.group==workflow.link.intent->group&&m.runtime!=workflow.link.runtime){choices.push_back(m.intent.member);group.addItem(100+int(choices.size()),"Peer "+juce::Uuid(m.intent.member.getRawData()).toString().substring(0,6));}
    menu.addSubMenu("Use group reference; share this EQ output",group,!choices.empty());juce::Component::SafePointer<EqMatchPanel> safe(&view);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&referenceKey),[safe,choices=std::move(choices)](int item){if(!safe||safe->state->retired)return;
        if(item==1)safe->state->chooseFile(false);else if(item==2)safe->state->chooseFile(true);else if(item>=10&&item<26)(void)safe->learn(true,(item-10)%2!=0,std::uint8_t((item-10)/2));else if(item>100&&std::size_t(item-100)<=choices.size())(void)safe->selectPeer(choices[std::size_t(item-101)]);
    });
}
void EqMatchPanel::State::chooseFile(bool save)
{
    if(retired)return;chooser=std::make_unique<juce::FileChooser>(save?"Export EQ reference":"Choose WAV or EQ reference",juce::File{},save?"*.eqref":"*.wav;*.eqref;*.json");
    juce::Component::SafePointer<EqMatchPanel> safe(&view);const auto flags=juce::FileBrowserComponent::canSelectFiles|(save?juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::warnAboutOverwriting:juce::FileBrowserComponent::openMode);
    chooser->launchAsync(flags,[safe,save](const juce::FileChooser& chooser){if(!safe||safe->state->retired)return;const auto keep=safe->state;const auto file=chooser.getResult();if(file==juce::File{})return;
        if(save){juce::String text;keep->message=safe->exportReference(text)&&file.replaceWithText(text)?"Reference exported; sound unchanged":"Could not export the reference";keep->sync();}
        else (void)safe->loadReferenceFile(file);
    });
}
EqMatchPanel::EqMatchPanel(InstrumentProcessor& p,const Model& m,std::shared_ptr<FxAnalysisHub> hub):state(std::make_shared<State>(*this,p,m,std::move(hub)))
{setComponentID("eq.match");setTitle("EQ Match");setWantsKeyboardFocus(true);setOpaque(true);}
EqMatchPanel::~EqMatchPanel(){retire();}
void EqMatchPanel::bind(DynamicEqGraph& graph)
{
    const auto keep=state;auto& s=*keep;if(s.retired)return;s.stopCapture();s.stopFile();s.invalidate();s.closePeer();s.source.reset();s.reference.reset();s.seconds=0;s.graph=&graph;s.slot=graph.boundSlot();
    if(!s.processor.trySoundSnapshot(s.sound,s.revision,s.reset)){s.slot.reset();s.message="Sound is busy; reopen Match";}else s.message="Learn this rack, then choose a reference";s.sync();
}
void EqMatchPanel::refresh(const SoundDocument&)
{
    const auto keep=state;auto& s=*keep;if(s.retired)return;
    if(!s.graph){s.stopCapture();s.stopFile();s.closePeer();s.slot.reset();s.source.reset();s.message="Rack unavailable; return to the rack";s.sync();return;}
    SoundDocument live;std::uint64_t revision,reset;
    if(!s.processor.trySoundSnapshot(live,revision,reset))return;
    if(reset!=s.reset||s.graph->boundSlot()!=s.slot)s.closePeer();
    if(revision!=s.revision||reset!=s.reset||s.graph->boundSlot()!=s.slot){s.stopCapture();s.stopFile();s.invalidate();s.source.reset();s.seconds=0;s.message="Sound changed; learn this rack again";}
    const auto rate=s.processor.fxMonitorHandle()->sampleRate();
    if(s.source&&rate>0&&rate!=s.source->sampleRate){s.stopCapture();s.invalidate();s.source.reset();s.seconds=0;s.message="Sample rate changed; learn this rack again";}
    s.sound=std::move(live);s.revision=revision;s.reset=reset;s.slot=s.graph->boundSlot();s.sync();
}
bool EqMatchPanel::learn(bool reference,bool output,std::optional<std::uint8_t> localSlot)
{
    const auto keep=state;auto& s=*keep;if(s.retired)return false;refresh(s.sound);if(!isVisible()||!s.graph||!s.slot)return false;
    const auto slot=localSlot.value_or(*s.slot);if(slot>=8||(!reference&&slot!=*s.slot)||s.sound.layout.effects[slot].use!=SlotUse::present||s.sound.layout.effects[slot].kind!=EffectKind::dynamicEq)return false;
    s.stopCapture();s.stopFile();s.invalidate();if(reference)s.closePeer();else{s.source.reset();s.sourceOutput=output;}
    const auto lease=s.hub->acquire(slot);if(!lease){s.message="Analyzer unavailable";s.sync();return false;}
    State::Capture capture;capture.lease=*lease;capture.reference=reference;capture.output=output;capture.slot=slot;capture.activity=juce::Time::getMillisecondCounterHiRes();
    const auto stats=s.processor.fxMonitorHandle()->stats(slot);capture.dropped=stats.dropped;capture.clipped=stats.clipped;capture.producer=stats.capturedFrames;s.capture=capture;s.seconds=0;s.message="Learning; at least 2 seconds of continuous audio";s.sync();return true;
}
bool EqMatchPanel::stopLearning()
{
    const auto keep=state;auto& s=*keep;if(s.retired)return false;refresh(s.sound);if(!s.capture)return false;const auto& c=*s.capture;
    if(c.seconds<2||!c.samples){s.message="Keep learning: 2 seconds of valid audio are needed";s.sync();return false;}
    EqReference r;r.sampleRate=c.rate;r.label="EQ "+juce::String(c.slot+1)+(c.output?" output":" input")+" / "+juce::String(c.seconds,1)+" s";r.captureId=juce::Uuid().toString();r.valid=c.valid;double maximum=-120;
    for(std::size_t i=0;i<129;++i){r.spectrum.db[i]=float(std::clamp(10*std::log10(std::max(1e-12,c.energy[i]/double(c.samples))),-120.,120.));if(r.valid[i])maximum=std::max(maximum,double(r.spectrum.db[i]));}
    const bool reference=c.reference;s.stopCapture();if(maximum<=-90){s.message="No usable audio; learn again";s.sync();return false;}
    if(reference)s.reference=r;else{s.source=r;s.sourceSeconds=s.seconds;}s.message=reference?"Reference frozen; Preview writes no sound":"Source frozen; choose a reference and Preview";s.sync();return true;
}
bool EqMatchPanel::learning() const noexcept{return !state->retired&&state->capture.has_value();}
double EqMatchPanel::learnedSeconds() const noexcept{return state->seconds;}
std::optional<EqReference> EqMatchPanel::sourceReference() const{return state->retired?std::optional<EqReference>{}:state->source;}
std::optional<EqReference> EqMatchPanel::reference() const{return state->retired?std::optional<EqReference>{}:state->reference;}
bool EqMatchPanel::selectPeer(GroupUuid member)
{
    const auto keep=state;auto& s=*keep;if(s.retired||!s.slot)return false;const auto workflow=s.processor.groupWorkflowView();
    if(!workflow.link.intent||(workflow.link.phase!=GroupLinkPhase::authorized&&workflow.link.phase!=GroupLinkPhase::admitted))return false;
    const auto found=std::find_if(workflow.members.begin(),workflow.members.end(),[&](const auto& m){return m.intent.member==member&&m.runtime!=workflow.link.runtime&&m.intent.group==workflow.link.intent->group;});if(found==workflow.members.end())return false;
    s.stopCapture();s.stopFile();s.invalidate();s.closePeer();const auto owner=s.processor.openEqSpectrumView();
    if(!owner||!s.processor.configureEqSpectrumView(owner,true,*s.slot,std::span(&member,1))){if(owner)s.processor.closeEqSpectrumView(owner);return false;}
    s.peerOwner=owner;s.peer=member;s.peerLease=s.hub->acquire(*s.slot);if(!s.peerLease){s.closePeer();return false;}
    s.reference.reset();(void)s.capturePeer();s.message="Explicit group reference; sharing this EQ output while Match is open";s.sync();return true;
}
bool EqMatchPanel::setReference(const EqReference& reference)
{
    const auto keep=state;auto& s=*keep;if(s.retired)return false;juce::String checked;if(!encodeEqReference(reference,checked))return false;
    s.stopCapture();s.stopFile();s.invalidate();s.closePeer();s.reference=reference;s.message="Reference selected; Preview writes no sound";s.sync();return true;
}
bool EqMatchPanel::loadReferenceFile(const juce::File& file)
{
    const auto keep=state;auto& s=*keep;if(s.retired||!s.graph||!s.slot)return false;s.stopCapture();s.stopFile();s.invalidate();s.closePeer();
    try{if(!s.pool)s.pool=std::make_unique<juce::ThreadPool>(1);s.job=std::make_unique<State::FileJob>(file);s.pool->addJob(s.job.get(),false);}catch(...){s.stopFile();s.message="Could not start reference analysis";s.sync();return false;}
    s.message="Analyzing reference; sound unchanged";s.sync();return true;
}
bool EqMatchPanel::loadingReference() const noexcept{return !state->retired&&bool(state->job);}
bool EqMatchPanel::importReference(const juce::String& text){EqReference r;return decodeEqReference(text,r)&&setReference(r);}
bool EqMatchPanel::exportReference(juce::String& text) const{return !state->retired&&state->reference&&state->peerCurrent()&&encodeEqReference(*state->reference,text);}
bool EqMatchPanel::preview(float amount)
{
    const auto keep=state;auto& s=*keep;if(s.retired)return false;refresh(s.sound);if(!s.graph||s.capture||s.job||!s.source)return false;
    if(!std::isfinite(amount)||amount<0||amount>1){s.invalidate();s.message="Amount must be between 0 and 100 percent";s.sync();return false;}
    if(s.peer&&!s.capturePeer()){s.invalidate();s.message="Peer unavailable; choose a fresh reference";s.sync();return false;}
    if(!s.reference)return false;s.amount.setValue(amount*100,juce::dontSendNotification);
    RoutedPatch patch;if(!s.slot||!makeRoutedPatch(s.sound,patch))return false;s.previewBypassed=patch.effects[*s.slot].bypassed;
    auto source=*s.source;
    if(!s.sourceOutput||s.previewBypassed){
        // The fitter adds bands to the retained curve; compare its modeled output to the reference.
        PreparedDynamicEqResponse response;if(!response.prepare(patch.effects[*s.slot].dynamicEq,source.sampleRate))return false;
        for(std::size_t i=0;i<129;++i)if(source.valid[i]){
            const auto energy=response.energy(frequency(i));if(!energy){source.valid.reset(i);continue;}
            const auto db=source.spectrum.db[i]+20*std::log10(std::max(*energy,1e-12));
            if(!std::isfinite(db)||db< -120||db>120)source.valid.reset(i);else source.spectrum.db[i]=float(db);
        }
    }
    const bool ready=s.graph->previewMatch(source,*s.reference,amount);s.message=ready?"Fitting; no changes applied":"Preview unavailable: check audio coverage, locks, free bands and the current review";s.sync();return ready;
}
bool EqMatchPanel::apply()
{
    const auto keep=state;auto& s=*keep;if(s.retired)return false;refresh(s.sound);if(!s.graph||s.capture||s.job||!s.source||!s.reference||!s.peerCurrent())return false;
    const bool ok=s.graph->applyMatch();if(s.retired)return ok;
    if(ok){s.source.reset();s.message="Applied; one Undo restores the previous sound";auto callback=onClose;if(callback)callback();}if(!s.retired)s.sync();return ok;
}
void EqMatchPanel::cancelPreview(){const auto keep=state;auto& s=*keep;if(s.retired)return;s.stopCapture();s.stopFile();s.invalidate();s.message="Canceled; sound unchanged";s.sync();}
void EqMatchPanel::updateMonitoring()
{
    const auto keep=state;auto& s=*keep;if(s.retired||!isVisible()||!s.graph)return;refresh(s.sound);if(s.retired||!s.graph||!s.slot)return;
    if(s.capture){auto& c=*s.capture;const auto stats=s.processor.fxMonitorHandle()->stats(c.slot);const auto* result=s.hub->result(c.lease);const auto now=juce::Time::getMillisecondCounterHiRes();
        if(stats.capturedFrames!=c.producer){c.producer=stats.capturedFrames;c.activity=now;}
        const bool broken=stats.dropped!=c.dropped||stats.clipped!=c.clipped||(c.generation&&(s.hub->generation(c.lease)!=c.generation||!result||!result->valid||result->generation!=c.generation||result->continuity!=c.continuity));
        if(broken||now-c.activity>2500){s.stopCapture();s.invalidate();s.message=broken?"Capture interrupted; learn again":"No current audio; learn again";}
        else if(result&&result->valid&&result->windows!=c.windows){
            if(!c.generation){c.generation=result->generation;c.continuity=result->continuity;c.rate=result->sampleRate;c.start=result->endSample;c.windows=result->windows;}
            else if(result->sampleRate!=c.rate||result->endSample<c.end){s.stopCapture();s.message="Stream changed; learn again";}
            else{c.windows=result->windows;const auto elapsed=double(result->endSample-c.start)/c.rate;
                if(elapsed>10){if(!stopLearning()){s.stopCapture();s.message="Not enough continuous audio";}}
                else if(result->endSample>=c.start+FxSpectrum::fftSize){const auto snapshot=localSnapshot(*result,c.output);
                    if(!snapshot){s.stopCapture();s.message="Invalid analyzer data; learn again";}
                    else{c.valid=snapshot->valid;c.end=result->endSample;c.seconds=elapsed;++c.samples;for(std::size_t i=0;i<129;++i)if(c.valid[i])c.energy[i]+=std::pow(10.,snapshot->spectrum.db[i]/10);s.seconds=c.seconds;}}
            }
        }
    }
    if(s.job&&s.job->done.load(std::memory_order_acquire)){const auto result=s.job->result;s.stopFile();if(result){s.reference=result;s.message="File reference ready; Preview writes no sound";}else s.message=s.reference?"Reference could not be analyzed; previous reference retained":"Reference could not be analyzed; check WAV format, duration and audio";}
    if(s.peer){
        if(!s.reference)(void)s.capturePeer();
        if(!s.peerCurrent()){s.invalidate();s.message="Peer reference unavailable or changed; Preview again when fresh";}
        if(const auto* current=s.peerLease?s.hub->result(*s.peerLease):nullptr;current&&current->valid){
            const auto levels=sampleEqSpectrum(*current,true);std::array<std::int16_t,128> payload;for(std::size_t i=0;i<128;++i)payload[i]=std::int16_t(std::lround(levels[i]*100));(void)s.processor.captureEqSpectrum(s.peerOwner,*s.slot,current->generation,current->windows,current->sampleRate,payload);}
    }
    s.graph->updateMonitoring();const auto fit=s.graph->matchPreview();if(fit.active)s.message=fit.fitting?"Fitting; no changes applied":fit.canApply?(s.previewBypassed?"Preview ready; rack remains bypassed":"Preview ready; sound unchanged"):fit.status;s.sync();
}
void EqMatchPanel::retire()
{
    if(!state||state->retired)return;const auto keep=state;auto& s=*keep;s.retired=true;s.stopCapture();s.stopFile();s.invalidate();s.closePeer();s.chooser.reset();s.pool.reset();s.graph=nullptr;onClose=nullptr;
    for(auto* key:{&s.back,&s.spectra,&s.correction,&s.sourceKey,&s.learnKey,&s.referenceKey,&s.load,&s.cancel,&s.previewKey,&s.applyKey})key->onClick=nullptr;s.amount.onValueChange=nullptr;
}
juce::String EqMatchPanel::statusText() const{return state->retired?"Match closed":state->message;}
juce::Rectangle<float> EqMatchPanel::plotBounds() const{return {96,122,884,178};}
void EqMatchPanel::paint(juce::Graphics& g)
{
    auto& s=*state;g.fillAll(juce::Colour(0xff111416));s.chassis.drawFace(g,{26,0,1028,526});for(int y=14;y<526;y+=28){g.setColour(juce::Colour(0xff050708));g.fillRoundedRectangle(5.f,float(y),14,17,1);g.fillRoundedRectangle(1061.f,float(y),14,17,1);}
    const juce::Rectangle<int> frame(54,28,972,460);g.setColour(juce::Colour(0xff0c191c));g.fillRoundedRectangle(frame.toFloat().reduced(3),3);drawDisplayRim(g,s.rim,frame);
    const auto text=[&](const juce::String& value,juce::Rectangle<int> r,float size=12.f,juce::Colour ink=paper,int align=juce::Justification::centredLeft){g.setColour(ink);g.setFont(juce::FontOptions(size));g.drawFittedText(value,r,align,1,1.f);};
    const auto fit=s.graph?s.graph->matchPreview():DynamicEqGraph::MatchPreview{};text("EQ MATCH / EQ "+juce::String(s.slot?int(*s.slot+1):0),{72,40,330,28},22,mint);text(fit.fitting?"FITTING - no changes applied":"PREVIEW - no changes applied",{578,40,326,28},12);
    g.setColour(grid);g.drawHorizontalLine(80,72,1000);text(s.correction.getToggleState()?"CORRECTION (dB)":"SPECTRA (dBFS)",{72,88,400,24});const auto plot=plotBounds();
    for(int n=0;n<5;++n){const auto y=plot.getY()+n*plot.getHeight()/4;g.setColour(grid);g.drawHorizontalLine(int(y),plot.getX(),plot.getRight());text(s.correction.getToggleState()?juce::String(12-n*6):juce::String(-n*30),{60,int(y)-9,30,18},11,paper,juce::Justification::centredRight);}
    for(double hz:{20.,50.,100.,200.,500.,1000.,2000.,5000.,20000.}){const auto x=plot.getX()+plot.getWidth()*float(std::log(hz/20)/std::log(1000.));g.setColour(grid);g.drawVerticalLine(int(x),plot.getY(),plot.getBottom());text(hz<1000?juce::String(int(hz)):juce::String(int(hz/1000))+"k",{int(x)-22,302,44,18},10,paper,juce::Justification::centred);}
    const auto trace=[&](const EqCurve& curve,juce::Colour ink,bool dashed,bool magnitude){juce::Graphics::ScopedSaveState saved(g);g.reduceClipRegion(plot.toNearestInt());juce::Path path;
        for(std::size_t i=0;i<129;++i){const auto x=plot.getX()+plot.getWidth()*float(i)/128;const auto db=curve.db[i];const auto y=magnitude?plot.getBottom()-plot.getHeight()*std::clamp((db+120)/120,0.f,1.f):plot.getCentreY()-plot.getHeight()*std::clamp(db,-12.f,12.f)/24;if(i==0)path.startNewSubPath(x,y);else path.lineTo(x,y);}g.setColour(ink);
        if(dashed){juce::Path line;const float dash[]{5,4};juce::PathStrokeType(1.4f).createDashedStroke(line,path,dash,2);g.fillPath(line);}else g.strokePath(path,juce::PathStrokeType(1.5f));};
    if(s.correction.getToggleState()){if(fit.target)trace(*fit.target,mint,false,false);if(fit.fit)trace(*fit.fit,amber,true,false);}
    else{if(s.source)trace(s.source->spectrum,mint,false,true);if(s.reference&&s.peerCurrent())trace(s.reference->spectrum,blue,true,true);}
    g.setColour(mint);g.drawLine(76,331,104,331,1.5f);text(s.correction.getToggleState()?"Target":"Source",{112,322,80,18},11);g.setColour(s.correction.getToggleState()?amber:blue);g.drawLine(206,331,234,331,1.5f);text(s.correction.getToggleState()?"Fit":"Reference",{242,322,110,18},11);
    text(s.message,{398,322,602,18},11,s.message.containsIgnoreCase("unavailable")||s.message.containsIgnoreCase("interrupted")?amber:paper.withAlpha(.8f),juce::Justification::centredRight);
    g.setColour(grid);g.drawRect(72,352,456,66);g.drawRect(544,352,456,66);text("THIS RACK",{84,357,250,18},11);text("REFERENCE",{556,357,350,18},11);
    text(s.capture&&!s.capture->reference?"Learning "+juce::String(s.seconds,1)+" s":s.source?"Captured "+juce::String(s.sourceSeconds,1)+" s":"No capture",{330,378,92,26},10);
    g.setColour(grid);g.drawHorizontalLine(430,72,1000);text("Amount",{72,444,68,28},13);text(juce::String(s.amount.getValue(),s.amount.getValue()==100?0:1)+"%",{276,444,62,28},13);
    text(juce::String(int(fit.added))+" / "+juce::String(int(fit.available))+" free bands - RMS "+(fit.rmsDb?juce::String(*fit.rmsDb,2)+" dB":"--"),{344,444,324,28},13);
}
void EqMatchPanel::resized(){auto& s=*state;s.back.setBounds(920,40,80,28);s.spectra.setBounds(784,88,100,24);s.correction.setBounds(896,88,104,24);s.sourceKey.setBounds(84,378,236,26);s.learnKey.setBounds(428,378,84,26);s.referenceKey.setBounds(556,378,336,26);s.load.setBounds(904,378,80,26);s.amount.setBounds(144,444,124,28);s.cancel.setBounds(688,444,96,28);s.previewKey.setBounds(796,444,96,28);s.applyKey.setBounds(904,444,96,28);}
void EqMatchPanel::visibilityChanged(){if(!state||state->retired)return;if(!isVisible()){cancelPreview();state->closePeer();}}
bool EqMatchPanel::keyPressed(const juce::KeyPress& key){if(key==juce::KeyPress::escapeKey){auto callback=onClose;if(callback)callback();return true;}return false;}
std::unique_ptr<juce::AccessibilityHandler> EqMatchPanel::createAccessibilityHandler(){return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::group);}
}
