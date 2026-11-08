#include "InstrumentProcessor.h"
#include "RackExamples.h"
#include "RackReview.h"
#include "StudioSkin.h"
#include "RackStack.h"
#include "HouseUnit.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <future>
#include <iostream>
#include <algorithm>
#include <filesystem>
#if JUCE_WINDOWS
#include <windows.h>
#endif

using namespace airack;
namespace {
bool plain(const juce::File& file) {
    for(auto p=file;;p=p.getParentDirectory()) {
        if(p.isSymbolicLink())return false;
#if JUCE_WINDOWS
        const auto flags=GetFileAttributesW(p.getFullPathName().toWideCharPointer());
        if(flags!=INVALID_FILE_ATTRIBUTES&&(flags&FILE_ATTRIBUTE_REPARSE_POINT))return false;
#endif
        if(p==p.getParentDirectory())return true;
    }
}
companion::ClientOptions offline() {
    companion::ClientOptions options;options.allowLaunch=false;
    options.launch.pipeSuffix="rack-preview-"+juce::Uuid().toString();return options;
}
std::vector<float> render(InstrumentProcessor& processor) {
    processor.setNonRealtime(true);processor.prepareToPlay(48000,128);
    std::vector<float> result;result.reserve(8192*2);
    for(int block=0;block<64;++block) {
        juce::AudioBuffer<float> audio(2,128);audio.clear();juce::MidiBuffer midi;
        if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(100)),0);
        if(block==32)midi.addEvent(juce::MidiMessage::noteOff(1,48),0);
        processor.processBlock(audio,midi);
        for(int frame=0;frame<128;++frame)for(int channel=0;channel<2;++channel)result.push_back(audio.getSample(channel,frame));
    }
    processor.releaseResources();return result;
}
struct Window final:juce::DocumentWindow {
    bool closed=false;
    Window():DocumentWindow("AIR rack developer preview",juce::Colour(0xff121619),allButtons){setUsingNativeTitleBar(true);setResizable(true,false);}
    void closeButtonPressed() override {closed=true;}
};
// Use the real rack controls without the application's session, feedback, AI or file-export surfaces.
struct PreviewPanel final:juce::Component,private juce::Timer,private juce::MidiKeyboardStateListener {
    InstrumentProcessor& processor;const studio::Model& model;
    juce::Viewport viewport;juce::Component content;RackStackUnit rack;
    juce::MidiKeyboardState keys;juce::MidiKeyboardComponent keyboard{keys,juce::MidiKeyboardComponent::horizontalKeyboard};
    juce::Label caption;
    PreviewPanel(InstrumentProcessor& p,const studio::Model& m,juce::Uuid id):processor(p),model(m),rack(p,m,id) {
        caption.setText("RACK DEVELOPER PREVIEW  /  local workspace  /  close and rebuild to refresh",juce::dontSendNotification);
        caption.setColour(juce::Label::textColourId,juce::Colour(0xffc8d3cb));
        addAndMakeVisible(caption);addAndMakeVisible(viewport);addAndMakeVisible(keyboard);
        viewport.setViewedComponent(&content,false);content.addAndMakeVisible(rack);viewport.setScrollBarsShown(true,false);
        keys.addListener(this);keyboard.setAvailableRange(24,96);keyboard.setLowestVisibleKey(36);tick();startTimerHz(30);
    }
    ~PreviewPanel() override {stopTimer();keys.removeListener(this);processor.releaseKeyboardNotes();}
    void handleNoteOn(juce::MidiKeyboardState*,int channel,int note,float velocity) override {processor.enqueueKeyboardNote(true,std::uint8_t(note),std::uint8_t(juce::roundToInt(127*velocity)),std::uint8_t(channel));}
    void handleNoteOff(juce::MidiKeyboardState*,int channel,int note,float) override {processor.enqueueKeyboardNote(false,std::uint8_t(note),0,std::uint8_t(channel));}
    void tick(){SoundDocument sound;std::uint64_t edit=0,reset=0;if(processor.trySoundSnapshot(sound,edit,reset))rack.refresh(sound);resized();}
    void timerCallback() override {tick();}
    void resized() override {
        auto bounds=getLocalBounds().reduced(8);caption.setBounds(bounds.removeFromTop(32));keyboard.setBounds(bounds.removeFromBottom(70));viewport.setBounds(bounds);
        const int base=int(model.layout["stage"][0]);if(base<=0)return;
        const float nativeScale=float(base)/(2.0f*studio::HouseUnit::railW+model.house.panelWidth);
        const int height=juce::roundToInt(rack.shownUnits()*model.house.unitHeight*nativeScale);
        const int width=std::max(1,viewport.getWidth()-18);const float scale=float(width)/float(base);
        rack.setSize(base,height);rack.setTransform(juce::AffineTransform::scale(scale));content.setSize(width,juce::roundToInt(height*scale));
    }
    void paint(juce::Graphics& g) override {g.fillAll(juce::Colour(0xff121619));}
};
int preview(const juce::File& archive,const juce::File& workspace,bool check) {
    if(!plain(archive)||!plain(workspace)||!archive.existsAsFile()||workspace.exists()
       ||archive.getSize()<=0||archive.getSize()>juce::int64(rackPackageLimits::archiveBytes)) {
        std::cerr<<"Refused: archive must be a regular file; workspace must be a new plain directory\n";return 2;
    }
    auto input=archive.createInputStream();if(!input)return 2;
    juce::ZipFile zip(*input);const auto index=zip.getIndexOfFileName("package.json");if(index<0)return 2;
    const auto* entry=zip.getEntry(index);if(!entry||entry->uncompressedSize>juce::int64(rackPackageLimits::manifestBytes))return 2;
    const std::unique_ptr<juce::InputStream> manifestStream(zip.createStreamForEntry(index));if(!manifestStream)return 2;
    const auto manifestText=manifestStream->readEntireStreamAsString();RackPackageManifest manifest;
    if(!decodeRackPackageManifest(manifestText.toStdString(),manifest))return 2;
    input->setPosition(0);manifest.ref.sha256=juce::SHA256(*input).toHexString();
    // One atomic directory creation refuses both existing paths and concurrent preview owners.
    if(!workspace.getParentDirectory().createDirectory())return 2;
    const auto path=std::filesystem::path(workspace.getFullPathName().toWideCharPointer());
    std::error_code error;if(!std::filesystem::create_directory(path,error))return 2;
    auto runtime=std::make_shared<RackLibraryRuntime>();runtime->store=std::make_shared<RackPackageStore>(RackLibraryPaths{workspace.getChildFile("library")});
    runtime->client=std::make_shared<RackLibraryClient>(runtime->store,[](const auto&,const auto&,const auto&,auto&){return RackLibraryStatus::networkFailure;});
    auto status=std::async(std::launch::async,[&]{return runtime->store->install(archive,manifest.ref,RackPackageActivation::makeActive,{});}).get();
    if(status!=RackLibraryStatus::ready){std::cerr<<"Native package admission failed: "<<int(status)<<'\n';return 1;}
    RackInventoryPtr inventory;if(!std::async(std::launch::async,[&]{return captureRackInventory(*runtime->store,{},inventory);}).get())return 1;
    RackDesign design;std::optional<RackPackageRef> ref;
    if(!resolveInventoryDesign(*inventory,manifest.metadata.design,manifest.metadata.revision,manifest.metadata.canonicalSha256,design,ref)||ref!=manifest.ref)return 1;
    InstrumentProcessor processor(offline(),runtime,InstrumentProcessor::RackPreviewMode{workspace});
    if(check&&(processor.mcpView().enabled||processor.soundFolder()!=workspace.getChildFile("Sounds"))) {
        std::cerr<<"Preview isolation check failed: external services or normal sound folder still enabled\n";return 1;
    }
    RackOperation operation;operation.design=design;operation.package=ref;operation.newInstance=juce::Uuid();operation.destination={design.role,masterChain,0};
    SoundDocument sound;RackDifference difference;
    if(makeRackCandidate(design.role==RackRole::fx?copperArcSound():newInstanceSound(),operation,SoundEditOrigin::manual,sound,difference)!=RackCandidateStatus::ready)return 1;
    juce::MemoryBlock state;if(!encodeSoundState(sound,state)||!processor.restoreSoundState(state.getData(),int(state.getSize())))return 1;
    processor.prepareRackSkins(inventory,sound.personalities->instances);
    const auto& rack=sound.personalities->instances.back();
    const auto deadline=juce::Time::getMillisecondCounter()+30000;
    while(manifest.skinPath.isNotEmpty()&&!processor.preparedMakerSkin(rack)) {
        if(juce::Time::getMillisecondCounter()>=deadline){std::cerr<<"Native skin preparation timed out\n";return 1;}
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    }
    if(check) {
        const auto expected=render(processor);InstrumentProcessor restored(offline(),runtime,InstrumentProcessor::RackPreviewMode{workspace});
        if(restored.mcpView().enabled||restored.soundFolder()!=workspace.getChildFile("Sounds"))return 1;
        if(!restored.restoreSoundState(state.getData(),int(state.getSize()))||render(restored)!=expected
           ||!std::all_of(expected.begin(),expected.end(),[](float sample){return std::isfinite(sample);}))return 1;
        if(!workspace.getChildFile("preview.soundstate").replaceWithData(state.getData(),state.getSize()))return 1;
        const auto* model=studio::studioSkin();if(!model)return 1;
        auto editor=std::make_unique<PreviewPanel>(processor,*model,rack.id);
        editor->addToDesktop(0);editor->setVisible(true);
        for(const auto size:{juce::Point<int>{1080,680},juce::Point<int>{1600,1000}}) {
            editor->setSize(size.x,size.y);juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            const auto file=workspace.getChildFile("preview-"+juce::String(size.x)+"x"+juce::String(size.y)+".png");
            auto output=file.createOutputStream();if(!output||!juce::PNGImageFormat().writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()),*output))return 1;
        }
        std::cout<<"PREVIEW PASS: native admission, editor, finite audio, exact recall; "<<manifest.ref.id<<' '<<manifest.ref.version<<' '<<manifest.ref.sha256<<'\n';return 0;
    }
    juce::AudioDeviceManager devices;juce::AudioProcessorPlayer player;player.setProcessor(&processor);
    const auto deviceError=devices.initialiseWithDefaultDevices(0,2);
    if(deviceError.isNotEmpty()){std::cerr<<deviceError<<'\n';player.setProcessor(nullptr);return 1;}
    devices.addAudioCallback(&player);devices.addMidiInputDeviceCallback({},&player);
    const auto* model=studio::studioSkin();if(!model)return 1;
    Window window;window.setContentOwned(new PreviewPanel(processor,*model,rack.id),true);window.centreWithSize(1080,680);window.setVisible(true);
    while(!window.closed)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    window.clearContentComponent();devices.removeMidiInputDeviceCallback({},&player);devices.removeAudioCallback(&player);player.setProcessor(nullptr);return 0;
}
}
int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;juce::String archive,workspace,parent;bool check=false;
    for(int n=1;n<argc;++n) {
        const juce::String key(argv[n]);if(key=="--check"){check=true;continue;}
        if(n+1>=argc)return 2;
        if(key=="--archive")archive=argv[++n];else if(key=="--workspace")workspace=argv[++n];else if(key=="--workspace-parent")parent=argv[++n];else return 2;
    }
    if(parent.isNotEmpty()){
        if(workspace.isNotEmpty()||!juce::File::isAbsolutePath(parent))return 2;
        workspace=juce::File(parent).getChildFile("preview-"+juce::Uuid().toString()).getFullPathName();
    }
    if(!juce::File::isAbsolutePath(archive)||!juce::File::isAbsolutePath(workspace))return 2;
    std::cout<<"PREVIEW_WORKSPACE "<<workspace<<'\n';
    try{return preview(juce::File(archive),juce::File(workspace),check);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
