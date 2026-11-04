#pragma once
#include "HouseControls.h"
#include "SpaceReverbControls.h"
#include <complex>

namespace airack::studio {
// Fixed identities and view state over one native slot. No audio preparation or monitoring.
class SpaceReverbGraph final : public juce::Component {
public:
    enum class Mode { decay, postEq };
    SpaceReverbGraph(InstrumentProcessor&, house::Host={});
    ~SpaceReverbGraph() override;
    void refresh(const SoundDocument&, std::uint8_t slot);
    void showProposal(std::shared_ptr<const SoundDocument>);
    void setMakerStyle(bool);
    void setMode(Mode);
    Mode mode() const;
    void selectBand(std::size_t);
    std::size_t selectedBand() const;
    std::optional<std::uint8_t> boundSlot() const;
    juce::Rectangle<float> plotBounds() const;
    juce::Point<float> nodePosition(std::size_t) const;
    std::optional<space::DecayPoint> decayAt(double, bool proposed=false) const;
    std::optional<std::complex<double>> postEqAt(double, bool proposed=false) const;
    // Solves the retained multiplier using only the common evaluator's log sensitivity.
    static std::optional<double> multiplierForTarget(space::DecayCurve, std::size_t band,
                                                     double frequency, double seconds, double sampleRate);
    std::vector<std::size_t> parameters() const;
    bool proposed() const;
    void suspend();
    void retire();
    std::function<void(Mode,std::size_t)> onBandSelected;
    std::function<void()> onEdited;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&,const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusLost(FocusChangeType) override;
    void visibilityChanged() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    struct State;
    std::shared_ptr<State> state;
    float pixelSize() const;
    int toolbarRows() const;
};
}
