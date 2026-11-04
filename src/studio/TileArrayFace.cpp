#include "TileArrayFace.h"
#include "MakerFace.h"
#include "DrumImport.h"
#include <algorithm>
#include <set>
#include <map>
#include <cmath>

namespace airack::studio {
namespace {
constexpr float nativeScale=2140.f/1520.f;
constexpr double faceX=2140.0/2066,faceY=788.0/761,revealX=2140.0/2069,revealY=788.0/760;
juce::Colour black(){return juce::Colour(0xff33252a);}
juce::Colour amber(){return juce::Colour(0xfff8cc7f);}
juce::Rectangle<float> rect(const juce::var& v){return {float(double(v[0])),float(double(v[1])),float(double(v[2])),float(double(v[3]))};}
house::Pt point(const juce::var& v){return {float(double(v[0])),float(double(v[1]))};}
float physical(const house::Model& m,const ParameterValues& values,const juce::String& key){
    for(const auto& e:m.elements)if(e.rackUnit>=0&&e.key==key)return parameterPhysicalValue(values,e.paramIndex);
    return 0;
}
void text(house::GeneratedText* renderer,juce::Graphics& g,const juce::String& value,float x,float y,float height,juce::Colour ink,bool bold=false,const char* family="Consolas",float width=1800){
    g.setColour(ink);g.setFont(juce::Font(juce::FontOptions(family,height,bold?juce::Font::bold:juce::Font::plain)));
    if(renderer&&renderer->draw(g,value,g.getCurrentFont(),ink,{x,y,width,height*1.35f},false))return;
    // The frozen review uses the Windows Consolas ascent (1884); JUCE's
    // portable metric uses the font's typographic ascent (1521), at em 2048.
    if(juce::String(family)=="Consolas")y+=height*(1884.f-1521.f)/2048.f;
    else if(juce::String(family)=="Bahnschrift"){x-=1;y-=1;}
    g.drawText(value,juce::Rectangle<float>(x,y,width,height*1.35f),juce::Justification::topLeft,false);
}
void sprite(house::GeneratedText* renderer,juce::Graphics& g,const house::Model& m,const juce::String& key,house::Pt at,house::Pt scale={1,1}){
    const auto& a=m.asset(key);if(renderer&&renderer->drawSprite(g,a.image,a.anchor,at,scale))return;g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImageTransformed(a.image,juce::AffineTransform::translation(-a.anchor.x,-a.anchor.y).scaled(scale.x,scale.y).translated(at.x,at.y));
}
juce::String pan(float value){const auto n=juce::roundToInt(std::abs(value)*100);return n==0?"C":juce::String(value<0?"L ":"R ")+juce::String(n);}
}

bool TileArrayFace::isTile(const house::Model& m){return m.designId==juce::Uuid("f97641c2-cd64-46a9-a3ab-0cc027a5f8d4");}
bool TileArrayFace::shows(const house::Element& e,std::size_t selected){return !e.group.startsWith("pad.")||e.group=="pad."+juce::String(int(selected)+1);}
void TileArrayFace::adaptModel(house::Model& m,const juce::var& doc){
    if(!isTile(m))return;
    if(m.units!=4||!doc["reveal"]["units_added"].isInt()||int(doc["reveal"]["units_added"])!=4
        ||m.reveal.kind!="drawer"||m.reveal.view!="programmer"||m.reveal.opener!="face.programmer"||!m.reveal.closers.contains("programmer.close"))
        throw std::runtime_error("unsupported Tile reveal");
    m.reveal.units=4;
    for(auto& e:m.elements)if(e.id=="face.enabled")e.stateCaptionAt=house::Pt(float(1784*faceX),float(399*faceY));
    const auto* records=doc["extension_records"].getArray();if(!records||records->size()!=30)throw std::runtime_error("Tile Array extensions missing");
    const std::map<juce::String,juce::String> supported{
        {"face.on","native-bound-readback"},{"face.content","native-drum-content-display"},{"face.programmer","reveal-action"},
        {"programmer.close","reveal-close-action"},{"programmer.import","owned-native-job-content-action"},
        {"programmer.generate_cancel","owned-native-job-content-action"},{"programmer.take.1","owned-native-job-content-action"},
        {"programmer.take.2","owned-native-job-content-action"},{"programmer.take.3","owned-native-job-content-action"},
        {"programmer.audition","owned-native-job-content-action"},{"programmer.keep","owned-native-job-content-action"},
        {"programmer.discard","owned-native-job-content-action"},{"programmer.content","native-owned-content-and-job-metadata"},
        {"programmer.prompt","native-bounded-prompt-field"}};
    std::set<juce::String> ids;std::array<bool,16> pads{};
    for(const auto& v:*records){
        const auto kind=v["proposed_kind"].toString(),id=v["id"].toString(),view=v["view"].toString();
        const bool pad=kind=="fixed-pad-performance-action";const auto declared=supported.find(id);
        if(!v.isObject()||!ids.insert(id).second||view!=(id.startsWith("face.")?"face":"programmer")
            ||(!pad&&(declared==supported.end()||declared->second!=kind)))throw std::runtime_error("unsupported Tile Array extension");
        const auto& plate=m.asset(view+":plate");const juce::Rectangle<float> area{plate.anchor.x,plate.anchor.y,float(plate.image.getWidth()),float(plate.image.getHeight())};
        if(kind!="native-owned-content-and-job-metadata"){
            const bool pivot=kind=="native-bound-readback";
            if(!v["geometry"][pivot?"pivot":"rect"].isArray())throw std::runtime_error("missing Tile geometry");
        }
        for(const auto* field:{"rect","pivot"})if(v["geometry"].hasProperty(field)){
            const auto* values=v["geometry"][field].getArray();if(!values||values->size()!=(juce::String(field)=="rect"?4:2))throw std::runtime_error("invalid Tile geometry");
            for(const auto& value:*values)if((!value.isInt()&&!value.isDouble())||!std::isfinite(double(value))||std::abs(double(value))>32768)throw std::runtime_error("invalid Tile coordinate");
            if(juce::String(field)=="rect"){const auto bounds=rect(v["geometry"][field]);if(bounds.isEmpty()||!area.contains(bounds))throw std::runtime_error("Tile rectangle outside plate");}
            else if(!area.contains(point(v["geometry"][field])))throw std::runtime_error("Tile pivot outside plate");
        }
        if(kind=="fixed-pad-performance-action"){
            const auto p=int(v["pad"]);if(!v["pad"].isInt()||p<1||p>16||pads[std::size_t(p-1)]||id!="face.pad."+juce::String(p)||view!="face"||!v["geometry"]["rect"].isArray()||!v["geometry"]["pivot"].isArray())throw std::runtime_error("invalid Tile pad");
            pads[std::size_t(p-1)]=true;
        }
    }
    if(std::find(pads.begin(),pads.end(),false)!=pads.end())throw std::runtime_error("missing Tile pad");
    m.nativeExtensionRecords=doc["extension_records"].clone();
    for(const auto& v:*records){
        const auto kind=v["proposed_kind"].toString();
        if(kind!="reveal-action"&&kind!="reveal-close-action"&&kind!="owned-native-job-content-action")continue;
        house::Element e;e.id=v["id"].toString();e.view=v["view"].toString();e.kind="action";e.label=v["label"].toString();
        if(e.id=="programmer.generate_cancel")e.label="GENERATE";
        if(e.id.startsWith("programmer.take."))e.label="TAKE "+e.id.fromLastOccurrenceOf(".",false,false);
        e.rect=rect(v["geometry"]["rect"]);e.hitRect=e.rect;e.group=e.view+".actions";
        e.stateScale={e.rect.getWidth()/(160*nativeScale),e.rect.getHeight()/(56*nativeScale)};
        for(const auto* state:{"idle","hover","pressed","disabled","selected"}){
            const auto key="part:membrane-action-key."+juce::String(state);(void)m.asset(key);e.stateAssets[state]=key;
        }
        auto existing=std::find_if(m.elements.begin(),m.elements.end(),[&](const auto& item){return item.id==e.id;});
        if(existing!=m.elements.end()){
            if(existing->view!=e.view||existing->kind!=e.kind||existing->rect!=e.rect||existing->rackUnit>=0||existing->paramId.isNotEmpty()
                ||std::count_if(m.elements.begin(),m.elements.end(),[&](const auto& item){return item.id==e.id;})!=1)
                throw std::runtime_error("conflicting Tile action");
            *existing=std::move(e);
        }else m.elements.push_back(std::move(e));
    }
    house::Group actions{"programmer.actions","programmer","File actions",{}, {}};
    for(const auto& e:m.elements)if(e.kind=="action"){
        if(e.view=="programmer"){actions.members.add(e.id);actions.focusOrder.add(e.id);}
        else for(auto& group:m.groups)if(group.view==e.view){group.members.add(e.id);group.focusOrder.add(e.id);break;}
    }
    m.groups.push_back(std::move(actions));
    for(auto& group:m.groups)if(group.view=="face"){
        juce::StringArray order;for(int p=1;p<=16;++p)order.add("face.pad."+juce::String(p));
        order.addArray(group.focusOrder);group.focusOrder=std::move(order);break;
    }
}

void TileArrayFace::paintExtras(juce::Graphics& g,const house::Model& m,const ParameterValues& values,const juce::String& view,
                              std::size_t selected,const std::array<DrumPadReadback,16>* metadata,
                              const std::array<bool,16>* available,const juce::String& job,bool controls,house::GeneratedText* renderer){
    if(!isTile(m))return;
    const auto padKey="drum.pad."+juce::String(int(selected)+1)+".";
    const auto number=juce::String(int(selected)+1).paddedLeft('0',2);
    const auto ready=available==nullptr||(*available)[selected];
    const auto* sample=metadata?&(*metadata)[selected]:nullptr;
    if(view=="face"){
        for(const auto& v:*m.nativeExtensionRecords.getArray()){
            if(v["proposed_kind"].toString()=="native-bound-readback")
                sprite(renderer,g,m,physical(m,values,"enabled")>.5f?"part:enabled-indicator.on":"part:enabled-indicator.off",point(v["geometry"]["pivot"]));
            if(controls&&v["proposed_kind"].toString()=="fixed-pad-performance-action"){
                const auto index=std::size_t(int(v["pad"])-1);const auto at=point(v["geometry"]["pivot"]);
                sprite(renderer,g,m,"part:ivory-performance-pad.idle",at);if(index==selected)sprite(renderer,g,m,"part:selected-pad-outline.selected",at);
                text(renderer,g,juce::String(int(index)+1).paddedLeft('0',2),at.x-88,at.y-52,42,black(),true,"Bahnschrift");
                text(renderer,g,"N"+juce::String(juce::roundToInt(physical(m,values,"drum.pad."+juce::String(int(index)+1)+".midi_note"))),at.x+30,at.y+23,29,black());
            }
        }
        if(controls)text(renderer,g,"ENABLE",float(1784*faceX),float(399*faceY),35,black(),true,"Bahnschrift");
        text(renderer,g,"PAD "+number+"    MIDI "+juce::String(juce::roundToInt(physical(m,values,padKey+"midi_note"))).paddedLeft('0',3),float(1198*faceX),float(119*faceY),45,amber());
        const auto name=!ready?juce::String("UNAVAILABLE"):sample&&sample->metadata.asset?juce::String(sample->metadata.name):juce::String("NO SAMPLE");
        text(renderer,g,name,float(1198*faceX),float(177*faceY),47,amber(),false,"Consolas",700);
        text(renderer,g,"KIT "+juce::String(juce::roundToInt(physical(m,values,"level")*100))+"%    PAN "+pan(physical(m,values,"pan")),float(1198*faceX),float(242*faceY),34,amber());
    }else if(view=="programmer"){
        text(renderer,g,"PAD "+number+" / FILE + VOICE",float(145*revealX),float(27*revealY),42,black(),true,"Bahnschrift");
        const std::array<const char*,9> keys{"gain_db","pan","tune_semitones","trim_start","trim_end","attack_seconds","decay_seconds","midi_note","choke_group"};
        const std::array<double,5> xs{204,568,929,1292,1650};
        for(std::size_t i=0;i<keys.size();++i){
            const float v=physical(m,values,padKey+keys[i]);juce::String value;
            switch(i){case 0:value=juce::String(v,2)+" dB";break;case 1:value=pan(v);break;
                case 2:value=juce::String(v,2)+" st";break;case 3:case 4:value=juce::String(v*100,1)+" %";break;
                case 5:value=juce::String(v,3)+" s";break;case 6:value=v==0?"NATURAL":juce::String(v,3)+" s";break;
                case 7:value=juce::String(juce::roundToInt(v)).paddedLeft('0',3);break;default:value=v==0?"OFF":juce::String(juce::roundToInt(v));break;}
            text(renderer,g,value,float((xs[i%5]+54)*revealX),float(((i<5?215:397)-20)*revealY),36,black(),false,"Consolas",250);
        }
        text(renderer,g,!ready?"SOURCE: UNAVAILABLE":sample&&sample->metadata.asset?sample->metadata.source&&sample->metadata.source->kind==drums::SourceKind::generated?"SOURCE: GENERATED":"SOURCE: IMPORTED":"SOURCE: EMPTY",float(1604*revealX),float(311*revealY),29,amber(),false,"Consolas",340);
        text(renderer,g,sample&&sample->metadata.asset?"LENGTH: "+juce::String(double(sample->frames)/48000,3)+" s":"LENGTH: --",float(1604*revealX),float(362*revealY),29,amber());
        text(renderer,g,"JOB: "+(job.isEmpty()?juce::String("IDLE"):job),float(1604*revealX),float(413*revealY),29,amber(),false,"Consolas",340);
        text(renderer,g,metadata?"Local generator / describe one hit":"Local generation unavailable",float(349*revealX),float(492*revealY),35,amber());
    }
    if(controls)for(const auto& e:m.elements)if(e.view==view&&e.kind=="action"){
        const auto disabled=e.id!="face.programmer"&&e.id!="programmer.import"&&e.id!="programmer.close";
        sprite(renderer,g,m,e.stateAsset(disabled?"disabled":"idle"),e.rect.getCentre(),e.stateScale);
        g.setFont(juce::Font(juce::FontOptions("Bahnschrift",31,juce::Font::bold)));g.setColour(black());
        if(!renderer||!renderer->draw(g,e.label,g.getCurrentFont(),black(),e.rect,true))
            g.drawText(e.label,e.rect.translated(0,-2),juce::Justification::centred,false);
    }
}

class TileArrayFace::Pad final : public juce::Component {
public:
    Pad(TileArrayFace& owner,std::size_t index,const juce::var& record):owner_(owner),index_(index),pivot_(point(record["geometry"]["pivot"])),hit_(rect(record["geometry"]["rect"])){
        auto bounds=hit_;
        for(const auto* key:{"part:ivory-performance-pad.idle","part:ivory-performance-pad.hover","part:ivory-performance-pad.pressed","part:ivory-performance-pad.disabled","part:selected-pad-outline.selected"}){
            const auto& image=owner_.context_.model.asset(key);
            bounds=bounds.getUnion(juce::Rectangle<float>(float(image.image.getWidth()),float(image.image.getHeight())).withPosition(pivot_-image.anchor));
        }
        setBounds(bounds.expanded(3).getSmallestIntegerContainer());
        setComponentID("face.pad."+juce::String(int(index)+1));setWantsKeyboardFocus(true);setMouseClickGrabsKeyboardFocus(false);setExplicitFocusOrder(int(index)+1);
    }
    bool hitTest(int x,int y) override{return hit_.contains(house::Pt(float(x),float(y))+getPosition().toFloat());}
    void paint(juce::Graphics& g) override{
        const auto at=pivot_-getPosition().toFloat();const auto& m=owner_.context_.model;
        sprite(owner_.context_.generatedText,g,m,!isEnabled()?"part:ivory-performance-pad.disabled":pressed_?"part:ivory-performance-pad.pressed":hover_?"part:ivory-performance-pad.hover":"part:ivory-performance-pad.idle",at);
        if(owner_.selected_==index_)sprite(owner_.context_.generatedText,g,m,"part:selected-pad-outline.selected",at);
        text(owner_.context_.generatedText,g,juce::String(int(index_)+1).paddedLeft('0',2),at.x-88,at.y-52,42,black(),true,"Bahnschrift");
        const auto key="drum.pad."+juce::String(int(index_)+1)+".midi_note";float note=0;
        for(const auto& e:m.elements)if(e.key==key){auto& p=owner_.context_.processor.control(e.paramIndex);note=p.convertFrom0to1(p.getValue());break;}
        text(owner_.context_.generatedText,g,"N"+juce::String(juce::roundToInt(note)),at.x+30,at.y+23,29,black());
        if(hasKeyboardFocus(false)){g.setColour(house::accent);g.drawRoundedRectangle((hit_-getPosition().toFloat()).expanded(3),5,2);}
    }
    void mouseDown(const juce::MouseEvent& e) override{pressed_=owner_.triggerPad(index_,e.mods.isShiftDown());repaint();}
    void mouseUp(const juce::MouseEvent&) override{pressed_=false;repaint();}
    void mouseEnter(const juce::MouseEvent&) override{hover_=true;repaint();}
    void mouseExit(const juce::MouseEvent&) override{hover_=false;repaint();}
    void focusGained(FocusChangeType) override{keyHeld_=keyHeld_||juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey)||juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::returnKey);repaint();}
    void focusLost(FocusChangeType) override{keyHeld_=false;pressed_=false;repaint();}
    bool keyPressed(const juce::KeyPress& key) override{
        const auto code=key.getKeyCode();
        if(code==juce::KeyPress::spaceKey||code==juce::KeyPress::returnKey){if(!keyHeld_){keyHeld_=true;pressed_=owner_.triggerPad(index_,key.getModifiers().isShiftDown());repaint();}return true;}
        int next=int(index_);if(code==juce::KeyPress::leftKey)--next;else if(code==juce::KeyPress::rightKey)++next;
        else if(code==juce::KeyPress::upKey)next+=4;else if(code==juce::KeyPress::downKey)next-=4;else return false;
        owner_.selectPad(std::size_t(juce::jlimit(0,15,next)),true);return true;
    }
    bool keyStateChanged(bool) override{if(!juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey)&&!juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::returnKey)){keyHeld_=false;pressed_=false;repaint();}return false;}
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override{
        return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::button,
            juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press,[this]{owner_.triggerPad(index_);}));
    }
private:
    TileArrayFace& owner_;std::size_t index_;house::Pt pivot_;juce::Rectangle<float> hit_;bool hover_=false,pressed_=false,keyHeld_=false;
};

class TileArrayFace::Overlay final : public juce::Component {
public:
    Overlay(TileArrayFace& owner,juce::String view):owner_(owner),view_(std::move(view)){setInterceptsMouseClicks(false,false);}
    void paint(juce::Graphics& g) override{
        ParameterValues values{};for(const auto& e:owner_.context_.model.elements)if(e.rackUnit>=0)values[e.paramIndex]=owner_.context_.processor.control(e.paramIndex).getValue();
        paintExtras(g,owner_.context_.model,values,view_,owner_.selected_,&owner_.metadata_,&owner_.available_,owner_.job_,false,owner_.context_.generatedText);
    }
private:TileArrayFace& owner_;juce::String view_;
};

TileArrayFace::TileArrayFace(MakerFace& owner,house::Context& c,house::View& face,house::View& reveal,juce::Uuid rack,std::uint8_t unit)
    :owner_(owner),context_(c),face_(face),reveal_(reveal),rack_(rack),unit_(unit){
    faceText_=std::make_unique<Overlay>(*this,"face");revealText_=std::make_unique<Overlay>(*this,"programmer");
    faceText_->setBounds(face_.getLocalBounds());revealText_->setBounds(reveal_.getLocalBounds());
    face_.addAndMakeVisible(*faceText_);faceText_->toBack();reveal_.addAndMakeVisible(*revealText_);revealText_->toBack();
    for(const auto& v:*context_.model.nativeExtensionRecords.getArray())if(v["proposed_kind"].toString()=="fixed-pad-performance-action"){
        const auto index=std::size_t(int(v["pad"])-1);if(index>=pads_.size())throw std::runtime_error("invalid Tile Array pad");
        pads_[index]=std::make_unique<Pad>(*this,index,v);face_.addAndMakeVisible(*pads_[index]);
    }
    reveal_.onGestureEnd=[this]{if(pending_&&!face_.hasHeldGesture()&&!reveal_.hasHeldGesture()){const auto next=*pending_;pending_.reset();selectPad(next.first,next.second);}};
    face_.onGestureEnd=reveal_.onGestureEnd;
    if(auto* button=reveal_.button("import"))button->onAction=[this,button]{import(button);};
    if(auto* button=reveal_.button("generate_cancel"))button->onAction=[this,button]{generate(button);};
    for(std::size_t n=0;n<3;++n)if(auto* button=reveal_.button("take."+juce::String(int(n)+1)))button->onAction=[this,n,button]{const auto view=context_.processor.drumImport().view();if(context_.processor.selectDrumGenerationCandidate(view.session,n)&&owner_.onDrumSource)owner_.onDrumSource(view.target,button);refresh();};
    if(auto* button=reveal_.button("audition"))button->onAction=[this]{context_.processor.drumImport().audition();refresh();};
    if(auto* button=reveal_.button("keep"))button->onAction=[this]{if(context_.processor.drumImport().keep()&&owner_.onDrumSourceClose)owner_.onDrumSourceClose();refresh();};
    if(auto* button=reveal_.button("discard"))button->onAction=[this]{context_.processor.drumImport().cancel();if(owner_.onDrumSourceClose)owner_.onDrumSourceClose();refresh();};
    selectionChanged();refresh();
}
TileArrayFace::~TileArrayFace(){face_.onGestureEnd=nullptr;reveal_.onGestureEnd=nullptr;}
bool TileArrayFace::selectPad(std::size_t pad,bool focus){
    const auto frame=context_.frame();if(pad>=16||suspended_||context_.closing())return false;
    if(face_.hasHeldGesture()||reveal_.hasHeldGesture()){pending_=std::pair(pad,focus);return false;}
    selected_=pad;selectionChanged();if(focus&&pads_[pad])pads_[pad]->grabKeyboardFocus();return true;
}
void TileArrayFace::selectionChanged(){
    for(auto& slider:reveal_.sliders)slider->setVisible(shows(slider->element));
    for(auto& pad:pads_)if(pad)pad->repaint();
    owner_.showScreen();owner_.remark();faceText_->repaint();revealText_->repaint();
}
bool TileArrayFace::triggerPad(std::size_t pad,bool selectionOnly){
    const auto frame=context_.frame();if(pad>=16||suspended_||context_.closing())return false;
    selectPad(pad,true);if(selectionOnly)return false;
    DrumPadTarget target;DrumCommitError error;
    if(!context_.processor.captureDrumPerformanceTarget(rack_,unit_,std::uint8_t(pad),target,error)){
        context_.report(error==DrumCommitError::staleTarget?"Pad target changed; hit not queued":"Pad target unavailable; hit not queued");return false;
    }
    if(!context_.processor.enqueueDrumPad(target,100,1)){
        const auto status=context_.processor.drumPerformanceStatus();
        context_.report(status==DrumPerformanceStatus::queueFull?"Pad queue full; hit not queued":
                        status==DrumPerformanceStatus::staleTarget?"Pad target changed; hit not queued":
                        status==DrumPerformanceStatus::invalid?"Invalid pad hit; not queued":"Pad performance unavailable; hit not queued");return false;
    }
    return true;
}
void TileArrayFace::import(juce::Component* invoking){
    const auto frame=context_.frame();if(suspended_||context_.closing())return;
    DrumPadTarget target;DrumCommitError error;
    if(!context_.processor.captureDrumPadTarget(rack_,unit_,std::uint8_t(selected_),target,error)){context_.report("Pad edit was rejected; sound preserved");return;}
    if(owner_.onDrumSource)owner_.onDrumSource(target,invoking);
}
void TileArrayFace::generate(juce::Component* invoking){
    const auto frame=context_.frame();if(suspended_||context_.closing())return;
    const auto current=context_.processor.drumImport().view();
    if(current.generation&&current.target.rack==rack_&&current.target.unit==unit_&&current.phase!=DrumImportPhase::closed&&current.phase!=DrumImportPhase::kept){if(owner_.onDrumSource)owner_.onDrumSource(current.target,invoking);return;}
    DrumPadTarget target;DrumCommitError error;
    if(!context_.processor.captureDrumPadTarget(rack_,unit_,std::uint8_t(selected_),target,error)){context_.report("Captured pad unavailable or locked; sound preserved");return;}
    if(!context_.processor.drumImport().openGeneration(target)){context_.report("Busy: finish or cancel current source or AI work");return;}
    if(owner_.onDrumSource)owner_.onDrumSource(target,invoking);
}
void TileArrayFace::refresh(){
    if(context_.closing())return;
    bool changed=false;
    for(std::size_t p=0;p<16;++p){DrumPadTarget target;DrumPadReadback value;DrumCommitError error;
        const bool valid=context_.processor.captureDrumPerformanceTarget(rack_,unit_,std::uint8_t(p),target,error)&&context_.processor.readDrumPad(target,value,error);
        const bool metadataChanged=valid&&(value.metadata!=metadata_[p].metadata||value.frames!=metadata_[p].frames||value.channels!=metadata_[p].channels);
        const bool padChanged=valid!=available_[p]||metadataChanged;changed|=padChanged;available_[p]=valid;
        if(metadataChanged)metadata_[p]=std::move(value);
        if(pads_[p]&&(padChanged||pads_[p]->getTitle().isEmpty())){pads_[p]->setTitle("Pad "+juce::String(int(p)+1)+", "+(valid?metadata_[p].metadata.asset?juce::String(metadata_[p].metadata.name):juce::String("No sample"):juce::String("Unavailable")));pads_[p]->repaint();}
    }
    const auto input=context_.processor.drumImport().view();
    const auto next=input.phase==DrumImportPhase::closed?juce::String("IDLE"):input.phase==DrumImportPhase::ready?juce::String("PREPARED"):input.phase==DrumImportPhase::kept?juce::String("KEPT"):input.phase==DrumImportPhase::cancelling?juce::String("DRAINING"):input.phase==DrumImportPhase::error?juce::String("ERROR"):input.generation?input.phase==DrumImportPhase::generating?(input.generationStage==DrumGenerationStage::inspecting?juce::String("INSPECT "):juce::String("GENERATE "))+juce::String(int(input.generationOrdinal)+1)+"/3":juce::String("LOCAL SOURCE"):juce::String("MANUAL SOURCE");
    changed|=next!=job_;job_=next;
    const bool own=input.generation&&input.target.rack==rack_&&input.target.unit==unit_;
    if(auto* button=reveal_.button("generate_cancel")){button->setEnabled(!suspended_&&!context_.closing());button->setDescription("Open the configured local generator; execution identity unverified");}
    for(std::size_t n=0;n<3;++n)if(auto* button=reveal_.button("take."+juce::String(int(n)+1))){button->setEnabled(own&&input.candidates[n].available);button->setTitle("TAKE "+juce::String(int(n)+1));button->setDescription(own&&input.candidates[n].available?"Owned inspected candidate; explicit crop and gain still required":"No inspected candidate available");}
    if(auto* button=reveal_.button("audition"))button->setEnabled(input.canAudition);
    if(auto* button=reveal_.button("keep")){button->setEnabled(input.canKeep);button->setTitle(input.canKeep?"KEEP PAD "+juce::String(int(input.target.pad)+1).paddedLeft('0',2):"KEEP");}
    if(auto* button=reveal_.button("discard")){button->setEnabled(own&&input.phase!=DrumImportPhase::closed&&input.phase!=DrumImportPhase::kept);button->setDescription("Cancel and discard this captured batch; the service may finish its work");}
    if(changed){faceText_->repaint();revealText_->repaint();}
}
void TileArrayFace::valuesChanged(){
    faceText_->repaint();revealText_->repaint();for(auto& pad:pads_)if(pad)pad->repaint();
}
void TileArrayFace::suspend(){suspended_=true;pending_.reset();}
void TileArrayFace::resume(){suspended_=false;}
void TileArrayFace::paint(juce::Graphics& g,const ParameterValues& values,const juce::String& view,bool controls) const{
    paintExtras(g,context_.model,values,view,selected_,&metadata_,&available_,job_,controls,context_.generatedText);
}
}
