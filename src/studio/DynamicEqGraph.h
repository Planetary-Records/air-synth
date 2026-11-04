#pragma once
#include "HouseControls.h"
#include "DynamicEq.h"
#include "EqCurveFit.h"
#include "EqReferenceAnalysis.h"
#include "FxAnalysisHub.h"
#include <complex>
#include <functional>
#include <bitset>

namespace airack::studio
{
// One fixed native view over the registered physical slot. Selection, capture
// and audition are transient; edits use the processor's existing transactions.
class DynamicEqGraph final : public juce::Component, private juce::Timer
{
public:
    struct Clipboard {
        std::function<juce::String()> read;
        std::function<void(const juce::String&)> write;
    };
    DynamicEqGraph(InstrumentProcessor&,house::Host={},Clipboard={},std::shared_ptr<FxAnalysisHub> = {});
    ~DynamicEqGraph() override;
    void refresh(const SoundDocument&,std::uint8_t slot);
    void showProposal(std::shared_ptr<const SoundDocument>);
    void selectBand(std::size_t);
    std::size_t selectedBand() const;
    void setSelectedBands(std::bitset<8>);
    std::bitset<8> selectedBands() const noexcept;
    bool copySelectedBands(juce::String&) const;
    bool pasteBands(const juce::String&);
    bool storeComparison(bool bankB);
    bool recallComparison(bool bankB);
    void clearComparison();
    std::optional<std::uint8_t> boundSlot() const;
    juce::Rectangle<float> plotBounds() const;
    juce::Point<float> nodePosition(std::size_t) const;
    // Mixed placements use stereo-energy magnitude in the combined view;
    // component views return their ordered self-transfer (other input zero).
    std::optional<std::complex<double>> responseAt(double,bool proposed=false) const;
    std::optional<std::complex<double>> componentResponseAt(double,airack::DynamicEqPlacement,bool proposed=false) const;
    bool proposed() const;
    std::vector<std::size_t> parameters() const;
    void setSpectrumOn(bool);
    bool spectrumOn() const;
    void setDetectorAuditionMode(bool);
    bool detectorAuditionMode() const noexcept;
    void setGrabEnabled(bool);
    bool grabEnabled() const noexcept;
    void setSketchEnabled(bool);
    bool sketchEnabled() const noexcept;
    // Accepts a preview job; Apply remains unavailable until it finishes.
    bool previewSketch(const airack::EqCurve&);
    bool sketchFitting() const noexcept;
    bool sketchPreviewReady() const noexcept;
    std::optional<float> sketchFitError() const noexcept;
    std::optional<airack::EqCurve> sketchTarget() const;
    std::size_t sketchPointCount() const noexcept;
    bool applySketch();
    void cancelSketch();
    bool previewMatch(const EqReference& source,const EqReference& reference,float amount);
    bool applyMatch();
    void cancelMatch();
    struct MatchPreview {
        bool active=false,fitting=false,canApply=false;
        std::size_t available=0,added=0;
        std::optional<EqCurve> target,fit;
        std::optional<float> rmsDb;
        juce::String status;
    };
    MatchPreview matchPreview() const;
    std::size_t grabPeakCount() const noexcept;
    juce::Point<float> grabPeakPosition(std::size_t) const;
    std::optional<FxAnalysisHub::Lease> subscription() const;
    std::shared_ptr<FxAnalysisHub> analysisHub() const;
    const FxSpectrum::Result& spectrum() const;
    // Also used by fixed offline/native snapshot and contention checks.
    void updateMonitoring();
    void suspend();
    void retire();
    std::function<void(std::size_t)> onBandSelected;
    std::function<void()> onEdited;
    std::function<void()> onOverview;
    std::function<void()> onMatch;
    std::function<void()> onSpectral;
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
    void timerCallback() override;
    struct State;
    std::shared_ptr<State> state;
};
}
