#pragma once
#include "HouseControls.h"
#include "InstrumentProcessor.h"

namespace airack::studio {
class MakerFace;

// Tile's content and actions; every numeric attachment remains owned by its original house view.
class TileArrayFace final {
public:
    static bool isTile(const house::Model&);
    static void adaptModel(house::Model&,const juce::var&);
    static bool shows(const house::Element&,std::size_t selectedPad);
    static void paintExtras(juce::Graphics&,const house::Model&,const ParameterValues&,const juce::String& view,
                            std::size_t selectedPad=0,const std::array<DrumPadReadback,16>* metadata=nullptr,
                            const std::array<bool,16>* available=nullptr,const juce::String& job={},bool controls=true,
                            house::GeneratedText* text=nullptr);
    TileArrayFace(MakerFace&,house::Context&,house::View&,house::View&,juce::Uuid,std::uint8_t unit);
    ~TileArrayFace();
    std::size_t selectedPad() const { return selected_; }
    bool selectPad(std::size_t,bool focus=false);
    bool triggerPad(std::size_t,bool selectionOnly=false);
    void refresh();
    void valuesChanged();
    void suspend();
    void resume();
    void paint(juce::Graphics&,const ParameterValues&,const juce::String& view,bool controls=true) const;
    bool shows(const house::Element& e) const { return shows(e,selected_); }
private:
    class Pad;
    class Overlay;
    void selectionChanged();
    void import(juce::Component*);
    void generate(juce::Component*);
    MakerFace& owner_;
    house::Context& context_;
    house::View& face_;
    house::View& reveal_;
    juce::Uuid rack_;
    std::uint8_t unit_;
    std::size_t selected_=0;
    std::optional<std::pair<std::size_t,bool>> pending_;
    bool suspended_=false;
    std::array<DrumPadReadback,16> metadata_;
    std::array<bool,16> available_{};
    std::array<std::unique_ptr<Pad>,16> pads_;
    std::unique_ptr<Overlay> faceText_,revealText_;
    juce::String job_;
};
}
