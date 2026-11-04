#pragma once
#include "InstrumentProcessor.h"
#include "RackOrder.h"
#include "RackReview.h"
#include "StudioSkin.h"
#include "StudioServices.h"
#include "RackLibraryClient.h"
namespace airack::studio {
struct RackLibraryBrowserState {
    RackLibraryQuery query;
    std::optional<RackPackageRef> release;
    juce::Uuid design=juce::Uuid::null();
    std::uint64_t revision=0;
    juce::String canonicalSha256;
    int galleryScroll=0;
    bool detailOpen=false;
};
void clearRackLibraryFilters(RackLibraryBrowserState&);
/** Native control-side Gallery. Display rows never grant placement authority. */
class LibraryUnit final : public juce::Component {
public:
    struct Host {std::function<std::shared_ptr<void>()> frame;std::function<bool()> closing;};
    LibraryUnit(InstrumentProcessor&,const Model&,StudioServices,Host);
    ~LibraryUnit() override;
    void selectRack(juce::Uuid);
    void refresh();
    void cancel();
    void visibilityChanged() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress&) override;
    std::optional<StackPlacement> placement() const;
    const RackLibraryBrowserState& browserState() const;
    std::size_t matchingRows() const;
    std::uint64_t ownedImageBytes() const;
    std::function<void()> onDone;
    std::function<void(juce::Uuid,const juce::String&)> onPlaced;
private:
    struct Browser;
    std::unique_ptr<Browser> browser;
};
}
