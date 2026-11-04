#include "DrumSourcePopup.h"
#include <algorithm>
#include <charconv>
#include <cmath>
namespace airack::studio {
namespace {
bool frameNumber(const juce::String& text,std::uint64_t& value){const auto s=text.toStdString();if(s.empty())return false;const auto result=std::from_chars(s.data(),s.data()+s.size(),value);return result.ec==std::errc()&&result.ptr==s.data()+s.size();}
void identify(juce::Component& c,const char* id,const juce::String& title,int order){c.setComponentID(id);c.setName(title);c.setTitle(title);c.setExplicitFocusOrder(order);}
const char* candidateStatus(drums::ComfyAttemptStatus status){switch(status){case drums::ComfyAttemptStatus::unrun:return "Unrun";case drums::ComfyAttemptStatus::rejected:return "Error: rejected";case drums::ComfyAttemptStatus::submissionAmbiguous:return "Error: outcome unknown";case drums::ComfyAttemptStatus::remoteFailed:return "Error: service failed";case drums::ComfyAttemptStatus::unavailable:return "Error: unavailable";case drums::ComfyAttemptStatus::timeout:return "Error: timed out";case drums::ComfyAttemptStatus::cancelled:return "Canceled";case drums::ComfyAttemptStatus::storageFailure:return "Error: storage";case drums::ComfyAttemptStatus::allocationFailed:return "Error: allocation";case drums::ComfyAttemptStatus::stale:return "Error: captured pad changed";default:return "Error: source invalid";}}
}
class DrumSourcePopup::Overview final:public juce::Component {
public:
 drums::DrumSourceInfo source;std::uint64_t begin=0,end=0;bool inspected=false;
 std::function<void(std::uint64_t,std::uint64_t)> onSelection;
 std::uint64_t anchorFrame=0;
 Overview(){setWantsKeyboardFocus(true);identify(*this,"drum.source.overview","Whole source overview; exact selection uses source frame fields",1);}
 std::uint64_t frameAt(float x) const{return source.frames?std::min(source.frames-1,std::uint64_t(juce::jlimit(0.,1.,double(x)/std::max(1,getWidth()))*source.frames)):0;}
 void mouseDown(const juce::MouseEvent& e) override {if(inspected){anchorFrame=frameAt(e.position.x);if(onSelection)onSelection(anchorFrame,anchorFrame+1);}}
 void mouseDrag(const juce::MouseEvent& e) override {if(inspected&&onSelection){const auto frame=frameAt(e.position.x);onSelection(std::min(anchorFrame,frame),std::max(anchorFrame,frame)+1);}}
 void paint(juce::Graphics& g) override {
  g.fillAll(juce::Colour(0xff15171b));g.setColour(juce::Colour(0xff54575c));g.drawRect(getLocalBounds());if(!inspected||!source.frames||!source.buckets)return;
  const auto w=float(getWidth()-2),h=float(getHeight()-20);g.setColour(juce::Colour(0x404c98ba));if(begin<end&&end<=source.frames)g.fillRect(1+w*float(double(begin)/source.frames),1.f,w*float(double(end-begin)/source.frames),h);
  for(unsigned c=0;c<source.channels;++c){const float centre=1+h*(float(c)+.5f)/source.channels,scale=h*.44f/source.channels;g.setColour(juce::Colour(0xffe8bf79));
   for(unsigned j=0;j<source.buckets;++j){const auto& b=source.overview[c][j];const auto x=1+w*(float(j)+.5f)/source.buckets;g.drawVerticalLine(juce::roundToInt(x),centre-b.maximum*scale,centre-b.minimum*scale);}}
  g.setColour(juce::Colours::lightgrey);g.setFont(12.f);g.drawText("0 s",4,getHeight()-18,70,16,juce::Justification::left);g.drawText(juce::String(double(source.frames)/source.rate,3)+" s",getWidth()-110,getHeight()-18,106,16,juce::Justification::right);
 }
};
DrumSourcePopup::DrumSourcePopup(InstrumentProcessor& p):processor(p){
 setLookAndFeel(&look);setWantsKeyboardFocus(true);setComponentID("drum.source.popup");setName("Drum source selection");setFocusContainerType(FocusContainerType::keyboardFocusContainer);
 overview=std::make_unique<Overview>();addAndMakeVisible(*overview);overview->onSelection=[this](std::uint64_t first,std::uint64_t last){loading=true;start.setText(juce::String(first),false);end.setText(juce::String(last),false);loading=false;edit();};
 for(auto* l:{&title,&source,&geometry,&status,&selection}){addAndMakeVisible(*l);l->setColour(juce::Label::textColourId,juce::Colours::whitesmoke);l->setFont(juce::FontOptions(14));}
 identify(source,"drum.source.file","Whole source file name",0);identify(geometry,"drum.source.geometry","Whole source metadata",0);identify(selection,"drum.source.selection","Prepared source selection summary",0);identify(status,"drum.source.status","Source operation status",0);
 for(auto* e:{&keptName,&start,&end,&gain}){addAndMakeVisible(*e);e->setFont(juce::FontOptions(15));e->onTextChange=[this]{if(!loading)edit();};e->onReturnKey=[this]{edit();};e->onEscapeKey=[safe=juce::Component::SafePointer<DrumSourcePopup>(this)]{if(safe)safe->close();};}
 identify(keptName,"drum.source.name","Kept sample name, maximum 64 UTF-8 bytes",2);identify(start,"drum.source.start","Source START inclusive, unsigned source frame",3);identify(end,"drum.source.end","Source END exclusive, unsigned source frame",4);identify(gain,"drum.source.gain","Gain multiplier greater than zero and at most sixteen",5);
 int order=6;for(auto* b:{&prepare,&audition,&keep,&cancel}){addAndMakeVisible(*b);b->setExplicitFocusOrder(order++);}
 identify(prepare,"drum.source.prepare","Prepare selected source",6);identify(audition,"drum.source.audition","Audition prepared source before pad trim voice and effects",7);identify(keep,"drum.source.keep","Keep captured pad",8);identify(cancel,"drum.source.cancel","Cancel source selection",9);
 prepare.onClick=[this]{edit();processor.drumImport().prepare();refresh();};audition.onClick=[this]{processor.drumImport().audition();refresh();};keep.onClick=[this]{if(processor.drumImport().keep())close(false);else refresh();};cancel.onClick=[this]{close();};setSize(720,450);
 addChildComponent(prompt);prompt.setFont(juce::FontOptions(16));prompt.setMultiLine(false);identify(prompt,"drum.generation.prompt","One-hit prompt, 1 to 2048 UTF-8 bytes",1);
 prompt.onTextChange=[this]{generationError.clear();if(!loading)refresh();};prompt.onReturnKey=[this]{refresh();};prompt.onEscapeKey=[safe=juce::Component::SafePointer<DrumSourcePopup>(this)]{if(safe)safe->close();};
 addChildComponent(generate);identify(generate,"drum.generation.generate","Generate three candidates for the captured pad",2);generate.onClick=[this]{generateCandidates();};
 for(std::size_t n=0;n<candidates.size();++n){auto& button=candidates[n];addChildComponent(button);button.setLookAndFeel(&generationLook);button.setComponentID("drum.generation.candidate."+juce::String(int(n)+1));button.setExplicitFocusOrder(int(n)+3);button.onClick=[this,n]{if(processor.selectDrumGenerationCandidate(session,n))refresh();};}
}
DrumSourcePopup::~DrumSourcePopup(){retired=true;stopTimer();chooser.reset();geometry.setLookAndFeel(nullptr);for(auto& candidate:candidates)candidate.setLookAndFeel(nullptr);setLookAndFeel(nullptr);}
bool DrumSourcePopup::open(const DrumPadTarget& target,juce::Component* from){
 if(!processor.drumImport().open(target)){refresh();return false;}generationMode=false;geometry.setLookAndFeel(nullptr);generationError.clear();retired=false;userEditingFields=false;shownDraft=shownSourceFrames=0;opener=from;session=processor.drumImport().view().session;setVisible(true);toFront(true);startTimer(25);refresh();return true;
}
bool DrumSourcePopup::openGeneration(const DrumPadTarget& target,juce::Component* from){
 auto view=processor.drumImport().view();const bool existing=view.generation&&view.target==target&&view.phase!=DrumImportPhase::closed&&view.phase!=DrumImportPhase::kept;
 if(!existing&&!processor.drumImport().openGeneration(target))return false;
 generationMode=true;geometry.setLookAndFeel(&generationLook);generationTarget=target;generationError.clear();retired=false;userEditingFields=false;shownDraft=shownSourceFrames=0;opener=from;session=processor.drumImport().view().session;setVisible(true);toFront(true);startTimer(25);refresh();prompt.grabKeyboardFocus();return true;
}
void DrumSourcePopup::generateCandidates(){
 std::array<std::uint64_t,3> seeds{};for(std::size_t n=0;n<3;++n){do{seeds[n]=std::uint64_t(juce::Random::getSystemRandom().nextInt64())&std::uint64_t(INT64_MAX);}while(std::find(seeds.begin(),seeds.begin()+std::ptrdiff_t(n),seeds[n])!=seeds.begin()+std::ptrdiff_t(n));}
 DrumCommitError error;if(!processor.beginDrumGeneration(generationTarget,prompt.getText(),seeds,error))generationError=error==DrumCommitError::invalid?"Prompt must contain 1 to 2048 UTF-8 bytes without control characters":"Busy, draining or captured pad unavailable; current work preserved";
 refresh();
}
void DrumSourcePopup::chooseSource(){
 if(retired||!session)return;chooser=std::make_unique<juce::FileChooser>("Import isolated drum source",juce::File{},"*.wav;*.aiff;*.aif;*.flac",true);const auto captured=session;juce::Component::SafePointer<DrumSourcePopup> safe(this);
 chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe,captured](const juce::FileChooser& result){if(safe)safe->chooseComplete(result.getResult(),captured);});
}
void DrumSourcePopup::chooseSourceForTests(const juce::File& file){chooseComplete(file,session);}
void DrumSourcePopup::chooseComplete(const juce::File& file,std::uint64_t captured){if(retired||captured!=session||!isVisible())return;if(!file.existsAsFile()){close();return;}processor.drumImport().choose(file);refresh();}
void DrumSourcePopup::close(bool cancelWork){retired=true;stopTimer();if(cancelWork)processor.drumImport().cancel();session=0;chooser.reset();setVisible(false);if(opener&&opener->isShowing())opener->grabKeyboardFocus();if(onClosed)onClosed();}
void DrumSourcePopup::refresh(){
 const auto v=processor.drumImport().view();if(session&&v.session!=session){close(false);return;}loading=true;
 generationForm=generationMode&&std::any_of(v.candidates.begin(),v.candidates.end(),[](const auto& c){return c.available;});
 title.setText("PAD "+juce::String(int(v.target.pad)+1).paddedLeft('0',2)+" "+juce::String::charToString(0x2014)+" SOURCE SELECTION",juce::dontSendNotification);source.setText(v.sourceName,juce::dontSendNotification);source.setTooltip(v.sourceName);
 geometry.setText(v.inspected?juce::String(v.source.rate)+" Hz  /  "+juce::String(v.source.channels)+" channel(s)  /  "+juce::String(v.source.frames)+" frames  /  "+juce::String(double(v.source.frames)/v.source.rate,6)+" s  /  peak "+juce::String(v.source.peak,6):"Inspecting source metadata",juce::dontSendNotification);
 juce::String operationMessage=v.message;
 if(generationMode){
  title.setText("PAD "+juce::String(int(v.target.pad)+1).paddedLeft('0',2)+" / LOCAL GENERATOR",juce::dontSendNotification);
  const auto text=prompt.getText();const auto bytes=text.getNumBytesAsUTF8();const auto* raw=text.toRawUTF8();const bool valid=bytes&&bytes<=2048&&std::none_of(raw,raw+bytes,[](unsigned char c){return c<32||c==127;});
  const bool choosing=v.phase==DrumImportPhase::choosing;prompt.setEnabled(choosing);generate.setEnabled(choosing&&valid);
  if(choosing&&!valid)operationMessage="Enter 1 to 2048 UTF-8 bytes without control characters ("+juce::String(bytes)+" bytes). Configured recipe / execution identity unverified";
  if(v.phase==DrumImportPhase::generating)operationMessage=(v.generationStage==DrumGenerationStage::inspecting?juce::String("Inspecting "):juce::String("Generating "))+juce::String(int(v.generationOrdinal)+1)+"/3; configured recipe / execution identity unverified";
  if(generationError.isNotEmpty())operationMessage=generationError;
  for(std::size_t n=0;n<candidates.size();++n){const auto& c=v.candidates[n];auto& row=candidates[n];const auto identity="PAD "+juce::String(int(v.target.pad)+1).paddedLeft('0',2)+" candidate "+juce::String(int(n)+1);row.setName(identity);row.setTitle(identity);
   const auto state=c.available?juce::String("Available"):v.phase==DrumImportPhase::generating?n==v.generationOrdinal&&v.generationStage!=DrumGenerationStage::waiting?v.generationStage==DrumGenerationStage::inspecting?juce::String("Inspecting"):juce::String("Generating"):juce::String("Waiting"):juce::String(candidateStatus(c.status));row.setButtonText(juce::String(int(n)+1)+" / seed "+juce::String(c.seed)+" / "+state);row.setDescription(state+"; configured recipe / execution identity unverified");row.setEnabled(c.available&&v.phase!=DrumImportPhase::cancelling&&v.phase!=DrumImportPhase::kept);row.setToggleState(generationForm&&n==v.selectedCandidate,juce::dontSendNotification);
  }
 }
 const auto candidate=v.generation?v.selectedCandidate:std::size_t(3);
 if(v.inspected&&(shownSourceSession!=v.session||shownCandidate!=candidate||shownSourceFrames!=v.source.frames)){shownSourceSession=v.session;shownCandidate=candidate;shownSourceFrames=v.source.frames;shownDraft=0;userEditingFields=false;}
 if(!userEditingFields&&(shownDraft!=v.draft||v.inspected&&end.getText()=="0")){keptName.setText(v.keptName,false);start.setText(juce::String(v.begin),false);end.setText(juce::String(v.end),false);gain.setText(juce::String(v.gain,6),false);}if(v.inspected)shownDraft=v.draft;
 if(status.getText()!=operationMessage){status.setText(operationMessage,juce::dontSendNotification);status.setDescription(operationMessage);if(auto* a=status.getAccessibilityHandler())a->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);}
 const bool legalSelection=v.inspected&&v.source.rate&&v.begin<v.end&&v.end<=v.source.frames&&v.end-v.begin<=std::uint64_t(v.source.rate)*10;
 const auto selected=legalSelection?v.end-v.begin:0;const auto canonical=legalSelection?(selected*48000+v.source.rate-1)/v.source.rate:0;
 char multiplier[32]{};const auto gainText=std::to_chars(std::begin(multiplier),std::end(multiplier),v.gain);
 const auto gainValue=gainText.ec==std::errc()?juce::String::fromUTF8(multiplier,int(gainText.ptr-multiplier)):juce::String("invalid");
 selection.setText(legalSelection?(v.preparedFrames?juce::String("Prepared "):juce::String("Selected "))+juce::String(canonical)+" frames / "+juce::String(double(selected)/v.source.rate,4)+" s\nGain "+gainValue+" ("+(v.gain>0?juce::String(20*std::log10(v.gain),2)+" dB":"invalid")+") / "+(v.preparedFrames?"peak "+juce::String(v.preparedPeak,6):juce::String("prepare for peak")):juce::String::fromUTF8("Invalid selection \xe2\x80\x94 choose at most ten seconds using source frame fields"),juce::dontSendNotification);
 prepare.setEnabled(v.canPrepare);audition.setEnabled(v.canAudition);keep.setEnabled(v.canKeep);keep.setButtonText("Keep PAD "+juce::String(int(v.target.pad)+1).paddedLeft('0',2));
 overview->source=v.source;overview->begin=v.begin;overview->end=v.end;overview->inspected=v.inspected;overview->repaint();loading=false;resized();
}
void DrumSourcePopup::edit(){
 userEditingFields=true;
 std::uint64_t first=0,last=0;const auto text=gain.getText().toStdString();float multiplier=0;const auto result=std::from_chars(text.data(),text.data()+text.size(),multiplier);
 if(!frameNumber(start.getText(),first)||!frameNumber(end.getText(),last)||result.ec!=std::errc()||result.ptr!=text.data()+text.size()||!std::isfinite(multiplier)){
  processor.drumImport().edit(0,0,0,keptName.getText());status.setText("Enter unsigned source frames and a finite gain multiplier",juce::dontSendNotification);prepare.setEnabled(false);audition.setEnabled(false);keep.setEnabled(false);return;}
 processor.drumImport().edit(first,last,multiplier,keptName.getText());refresh();
}
void DrumSourcePopup::resized(){
 title.setBounds(18,10,getWidth()-36,27);source.setBounds(18,40,getWidth()-36,23);geometry.setBounds(18,65,getWidth()-36,23);overview->setBounds(18,94,getWidth()-36,100);keptName.setBounds(18,204,getWidth()-36,28);start.setBounds(18,244,210,30);end.setBounds(246,244,210,30);gain.setBounds(474,244,getWidth()-492,30);selection.setBounds(18,278,getWidth()-36,56);status.setBounds(18,335,getWidth()-36,60);int x=18;for(auto* b:{&prepare,&audition,&keep,&cancel}){b->setBounds(x,397,158,34);x+=174;}
 source.setVisible(!generationMode);prompt.setVisible(generationMode&&!generationForm);generate.setVisible(generationMode&&!generationForm);for(auto& row:candidates)row.setVisible(generationMode);
 const bool form=!generationMode||generationForm;geometry.setVisible(form);overview->setVisible(form);keptName.setVisible(form);start.setVisible(form);end.setVisible(form);gain.setVisible(form);selection.setVisible(form);
 if(generationMode&&generationForm){for(std::size_t n=0;n<3;++n)candidates[n].setBounds(18,40+int(n)*24,getWidth()-36,22);geometry.setBounds(18,112,getWidth()-36,20);overview->setBounds(18,134,getWidth()-36,60);}
 else if(generationMode){prompt.setBounds(18,45,getWidth()-36,58);generate.setBounds(getWidth()-218,112,200,34);for(std::size_t n=0;n<3;++n)candidates[n].setBounds(18,160+int(n)*34,getWidth()-36,30);status.setBounds(18,270,getWidth()-36,115);}
 repaint();
}
void DrumSourcePopup::paint(juce::Graphics& g){g.fillAll(juce::Colour(0xff24262b));g.setColour(house::accent);g.drawRect(getLocalBounds(),2);g.setFont(12.f);if(!generationMode||generationForm){g.drawText("KEPT NAME",18,190,200,14,juce::Justification::left);g.drawText("START inclusive (source frames)",18,232,220,14,juce::Justification::left);g.drawText("END exclusive (source frames)",246,232,220,14,juce::Justification::left);g.drawText("GAIN multiplier",474,232,220,14,juce::Justification::left);}}
bool DrumSourcePopup::keyPressed(const juce::KeyPress& key){if(key==juce::KeyPress::escapeKey){close();return true;}if(key==juce::KeyPress::returnKey){if(generationMode&&!generationForm)refresh();else edit();return true;}return false;}
}
