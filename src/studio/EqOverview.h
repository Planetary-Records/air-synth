#pragma once
#include "FxAnalysisHub.h"
#include "HouseControls.h"
#include "StudioSkin.h"
#include "GroupIntent.h"
#include <bitset>
#include <span>

namespace airack::studio
{
std::bitset<128> potentialEqOverlap(const std::array<float,128>&,const std::array<float,128>&) noexcept;
std::array<float,128> sampleEqSpectrum(const FxSpectrum::Result&,bool output) noexcept;
class EqOverview final : public juce::Component
{
public:
    EqOverview(InstrumentProcessor&,const Model&,std::shared_ptr<FxAnalysisHub>);
    ~EqOverview() override;
    void refresh(const SoundDocument&);
    void updateMonitoring();
    void retire();
    bool setReferenceSlot(std::uint8_t);
    bool setComparisonSlot(std::uint8_t);
    bool setComparisonPeer(GroupUuid member);
    bool setGroupEnabled(bool);
    bool setPeerSelection(std::span<const GroupUuid>);
    bool setSendSlot(std::uint8_t);
    std::optional<std::uint8_t> referenceSlot() const;
    std::size_t localCount() const noexcept;
    std::size_t peerCount() const noexcept;
    bool localAvailable(std::uint8_t) const;
    std::optional<std::bitset<128>> potentialOverlap() const;
    juce::String statusText() const;
    juce::Rectangle<float> plotBounds() const;
    std::function<void()> onClose;
    std::function<void(std::uint8_t)> onNavigate;
    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseWheelMove(const juce::MouseEvent&,const juce::MouseWheelDetails&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
