#pragma once
#include "DynamicEqGraph.h"
#include "StudioSkin.h"
#include "GroupIntent.h"

namespace airack::studio
{
class EqMatchPanel final : public juce::Component
{
public:
    EqMatchPanel(InstrumentProcessor&,const Model&,std::shared_ptr<FxAnalysisHub>);
    ~EqMatchPanel() override;
    void bind(DynamicEqGraph&);
    void refresh(const SoundDocument&);
    bool learn(bool reference,bool output,std::optional<std::uint8_t> localSlot={});
    bool stopLearning();
    bool learning() const noexcept;
    double learnedSeconds() const noexcept;
    std::optional<EqReference> sourceReference() const;
    std::optional<EqReference> reference() const;
    bool selectPeer(GroupUuid);
    bool setReference(const EqReference&);
    bool loadReferenceFile(const juce::File&);
    bool loadingReference() const noexcept;
    bool importReference(const juce::String&);
    bool exportReference(juce::String&) const;
    bool preview(float amount=1);
    bool apply();
    void cancelPreview();
    void updateMonitoring();
    void retire();
    juce::String statusText() const;
    juce::Rectangle<float> plotBounds() const;
    std::function<void()> onClose;
    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyPressed(const juce::KeyPress&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    struct State;
    std::shared_ptr<State> state;
};
}
