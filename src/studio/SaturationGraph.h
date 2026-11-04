#pragma once
#include "HouseControls.h"
#include "Saturation.h"

namespace airack::studio
{
struct MakerSkin;
class SaturationGraph final : public juce::Component
{
public:
    SaturationGraph(InstrumentProcessor&, house::Host = {},std::shared_ptr<const MakerSkin> equipment = {});
    ~SaturationGraph() override;
    void refresh(const SoundDocument&, std::uint8_t slot);
    void setEquipmentArtwork(std::shared_ptr<const MakerSkin>);
    void showProposal(std::shared_ptr<const SoundDocument>);
    std::vector<std::size_t> parameters() const;
    bool beginCrossoverGesture();
    bool dragCrossovers(float lowHz, float highHz);
    bool endCrossoverGesture();
    bool beginBandAudition(SaturationLane);
    void endBandAudition();
    void retire();
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusLost(FocusChangeType) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    friend struct SaturationGraphTestAccess;
    bool (*auditionKeyDown)(int)=juce::KeyPress::isKeyCurrentlyDown;
    struct State;
    std::shared_ptr<State> state;
};
}
