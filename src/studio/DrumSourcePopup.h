#pragma once
#include "DrumImport.h"
#include "HouseControls.h"
namespace airack::studio {
// A house form in editor pixels, above the scaled stage. The processor owns its work.
class DrumSourcePopup final : public juce::Component,private juce::Timer {
public:
 explicit DrumSourcePopup(InstrumentProcessor&);
 ~DrumSourcePopup() override;
 bool open(const DrumPadTarget&,juce::Component* invokingImport=nullptr);
 bool openGeneration(const DrumPadTarget&,juce::Component* invokingGenerate=nullptr);
 void chooseSource();
 // Test chooser uses the exact live chooser completion, including retirement checks.
 void chooseSourceForTests(const juce::File&);
 void close(bool cancel=true);
 void refresh();
 void resized() override;
 void paint(juce::Graphics&) override;
 bool keyPressed(const juce::KeyPress&) override;
 juce::TextEditor keptName,start,end,gain;
 juce::TextEditor prompt;
 juce::TextButton generate{"Generate"};
 std::array<juce::TextButton,3> candidates;
 juce::TextButton prepare{"Prepare"},audition{"Audition"},keep{"Keep"},cancel{"Cancel"};
 std::function<void()> onClosed;
private:
 class Overview;
 void timerCallback() override{refresh();}
 void chooseComplete(const juce::File&,std::uint64_t);
 void edit();
 void generateCandidates();
 InstrumentProcessor& processor;
 house::EditorLook look;
 struct GenerationLook final:juce::LookAndFeel_V4 {
  juce::Font getLabelFont(juce::Label&) override {return juce::Font(juce::FontOptions(16));}
  juce::Font getTextButtonFont(juce::TextButton&,int) override {return juce::Font(juce::FontOptions(16));}
 } generationLook;
 juce::Label title,source,geometry,status,selection;
 std::unique_ptr<Overview> overview;
 std::unique_ptr<juce::FileChooser> chooser;
 juce::Component::SafePointer<juce::Component> opener;
 std::uint64_t session=0,shownDraft=0,shownSourceFrames=0,shownSourceSession=0;
 std::size_t shownCandidate=3;
 bool loading=false,retired=false,userEditingFields=false;
 bool generationMode=false,generationForm=false;
 DrumPadTarget generationTarget;
 juce::String generationError;
};
}
