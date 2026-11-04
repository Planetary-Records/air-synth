#pragma once
#include "RackPanel.h"
#include "RackOrder.h"
#include "AllControlsPanel.h"
#include "HouseControls.h"
#include "MakerFace.h"
#include "ProgrammerLook.h"
#include <memory>
#include <optional>
#include <utility>
#include <vector>
namespace airack::studio { class HouseUnit; class EmptyRackSpace; class DynamicEqGraph; class SpaceReverbGraph; class FxAnalysisHub; class FxAnalysisClock; class EqOverview; class EqMatchPanel; }
namespace airack {
class InstrumentProcessor;
struct DrumPadTarget;
// One placed rack: its 2U plate with the identity block, its keys and its row, its maker's faceplate with OPERATIONS
// on the right rail (the maker faceplates spec), or folded its 1U fallback strip, or the faceplate's own strip where its
// skin has one (the Glass Current faceplate spec). Its tapes, screws, triangle and faceplate are drawn in the house
// unit's design pixels at the stack's scale; the rest in the stack's pixels. For a bank no rack holds (bareBankId) it is
// a generic rack: the house plate lettered with the bank, with OPERATIONS and no UP, DOWN, row or faceplate.
class RackStackUnit final : public juce::Component {
public:
 RackStackUnit(InstrumentProcessor&,const studio::Model&,juce::Uuid,studio::house::Host={},std::shared_ptr<studio::FxAnalysisHub> = {});
 ~RackStackUnit() override;
 juce::Uuid id() const;
 // The labels, keys, tapes, LED and lock, and the row; the plate is drawn again only when the name, LED or lock changes.
 // A move key about to be disabled hands its focus to the other move key, or to the triangle. The faceplate is made
 // while makerFaceFor gives the rack's design one, and made again when its skin or the rack's bindings change; a
 // dropped faceplate hands its focus to the triangle.
 void refresh(const SoundDocument&);
 // Folded, the strip hides the name, place, keys and row, an open SET-UP plate, drawer or door closes, and focus inside
 // them moves to the triangle; the faceplate folds to its own strip, or hides for the fallback strip.
 void setFolded(bool);
 // The narrow-width rule: shown folded while the column folds it, as view state, its own fold kept; the focus inside
 // it leaves it rather than going to its triangle, which would choose it again.
 void setColumnFolded(bool);
 bool columnFolded() const;
 // The user's own fold.
 bool foldedByUser() const;
 // Shown folded, by its own fold or the column's.
 bool isFolded() const;
 // Open in a column narrower than its plate: how far its plate's parts are shifted left, in stack px, kept from 0 to
 // across(), how far the plate reaches past the right rail's inner edge while it is shown unfolded, else 0.
 void setShift(int);
 int shift() const;
 int across() const;
 // The plate's parts as they shift, in "rack.pane": the faceplate, or the house plate's labels, keys and row.
 juce::Component& plate();
 // 2, the faceplate's units (with an open drawer's or door's), or 1 folded.
 int units() const;
 // As units(), with a moving drawer's or door's units at its drawn fraction.
 float shownUnits() const;
 // As lettered: "EMBER REED", and "SYNTH 1" or "SYNTH 1 INSERT | EDITED".
 juce::String name() const;
 juce::String place() const;
 // The folded strip's LED: lit while none of the rack's banks is disabled or bypassed.
 bool on() const;
 // The AI: KEEP tape shows: one of the rack's banks is AI-locked.
 bool kept() const;
 // The proposal's candidate sound for the faceplate, or else the row, or none. Their marks or outline make the folded
 // fallback strip's tape outline (a faceplate's own strip outlines its display), and the outline adds "AI proposes a new
 // panel" to the unit's description.
 void setProposal(std::shared_ptr<const SoundDocument>);
 // The faceplate or the row marks a control or is outlined, or hiddenProposal().
 bool proposed() const;
 // A change only the drawer shows: the candidate keeps the rack on its banks (a generic rack's bank present and
 // unowned) and changes a parameter in its units' reach that is not locked live and is on neither its row nor its
 // faceplate. While it holds and the drawer is closed, dashes run round OPERATIONS, and the description adds "AI
 // proposes changes in the drawer".
 bool hiddenProposal() const;
 // The faceplate, folded too, or null on the house plate.
 studio::MakerFace* face() const;
 studio::DynamicEqGraph* eqGraph() const;
 studio::SpaceReverbGraph* spaceGraph() const;
 void retireGraph();
 // AI: KEEP, over the ear and the rail's edge, passing under the rail as the plate shifts; folded to the fallback strip
 // while proposed, dashes round the name's tape.
 void paintOverChildren(juce::Graphics&) override;
 void resized() override;
 // A press on the triangle while the column folds the unit, marked before the stack's press listener opens it.
 void mouseDown(const juce::MouseEvent&) override;
 void mouseUp(const juce::MouseEvent&) override;
 std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
 // After a fold or the faceplate's drawer or door changes the unit's height.
 std::function<void()> onLayout;
 // After the faceplate's reveal opens: the stack shows what it opened (MakerFace::openedArea), or a recess.
 std::function<void()> onOpened;
 // A faceplate SET-UP opens this rack's existing all-controls drawer.
 std::function<void()> onSetup;
 // The triangle pressed while the column folds the unit: the stack chooses it, which opens it.
 std::function<void()> onChosen;
 // A horizontal wheel, or Shift and the wheel, over the open plate off its controls: how far to shift it, in stack px.
 std::function<void(int)> onAcross;
 std::function<void(std::size_t)> onBandSelected;
 std::function<void(bool postEq,std::size_t)> onSpaceBandSelected;
 std::function<void()> onEdited;
 std::function<void()> onOverview;
 std::function<void()> onMatch;
 std::function<void()> onSpectral;
 std::function<void(DrumPadTarget,juce::Component*)> onDrumSource;
 std::function<void()> onDrumSourceClose;
 studio::house::FoldRail fold;
 studio::KeyFocus<juce::TextButton> up,down,operations;
 RackPanelComponent row;
private:
 // The header, the faceplate's maker and model, and the faceplate's or row's outline as the description; the unit drawn
 // again when proposed() changes.
 void describe();
 // hiddenProposal() again from the candidate and the live values refresh kept; the unit drawn again when it changes.
 void rehide();
 void makeFace(studio::MakerSkinPtr,const RackInstance&);
 // The faceplate leaves the unit now and is deleted on the next turn, as a retired unit is.
 void dropFace();
 // The identity block, the keys, the row and the faceplate shown as the fold and the faceplate have them.
 void showParts();
 void layoutSpaceGraph();
 // Shown folded or not after either fold, from `was`: the focus, the faceplate's fold, the shift, the triangle, the parts.
 void showFold(bool was,bool byColumn);
 // The house plate across the panel, or folded the fallback strip across the column, under the parts.
 void paintPlate(juce::Graphics&);
 struct Pane;
 struct Track;
 const studio::Model& model;
 studio::ProgrammerLook look;
 juce::Uuid rack;
 std::optional<BareBank> bare;
 juce::Label nameLabel,placeLabel;
 juce::Image screw;
 // The house's mark: printed under the identity block's keys, carved into the fallback strip.
 juce::Path mark;
 // The design's name in its own case, for the titles; rackHeaderText, for the description.
 juce::String designName,header;
 std::optional<studio::house::TapeArt> nameTape;
 float tapeWidth=180.0f;
 studio::house::TapeArt keepTape;
 bool folded=false,lit=true,locked=false,marked=false;
 InstrumentProcessor& processor;
 studio::house::Host host;
 std::shared_ptr<studio::FxAnalysisHub> analysis;
 // OPERATIONS's look on the right rail.
 std::unique_ptr<juce::LookAndFeel> railKey;
 std::unique_ptr<studio::MakerFace> maker;
 std::unique_ptr<studio::DynamicEqGraph> graph;
 std::unique_ptr<studio::SpaceReverbGraph> spaceGraphView;
 // The rack's bindings the faceplate was made for.
 std::vector<UnitBinding> bound;
 std::shared_ptr<const SoundDocument> candidate;
 // The units' reach, kept until the units change; of it, what only the drawer shows, with its live values; the
 // rack's live bindings.
 std::vector<studio::AllControlsPanel::Unit> reachUnits;
 std::vector<std::size_t> reach;
 std::vector<std::pair<std::size_t,float>> drawerOnly;
 std::vector<UnitBinding> liveBindings;
 bool hidden=false;
 bool columnFold=false,foldPress=false;
 int shiftPx=0;
 std::unique_ptr<Pane> pane;
 std::unique_ptr<Track> track;
};
// The rack area's stack (the rack stage spec): the house unit, each placed rack, the shared-master divider and the empty
// space on numbered rails, in a viewport over a body laid out in stage pixels across the stack's own width, the stage's
// or, narrower, a narrow column (the narrow-width rule spec).
class RackStack final : public juce::Component, private juce::FocusChangeListener, private juce::ScrollBar::Listener, private juce::Timer {
public:
 // The host's frame, closing, reports and edits go to each faceplate's controls.
 RackStack(InstrumentProcessor&,const studio::Model&,studio::HouseUnit&,studio::EmptyRackSpace&,RackPanelComponent& drawer,studio::house::Host={},std::shared_ptr<studio::FxAnalysisHub> = {});
 ~RackStack() override;
 // Units made, kept or retired by rackOrder, and laid out again only when the order changes; the selection kept or
 // given to the first placed rack, never to a generic one; the drawer closed when its rack is folded, unselected or gone.
 void refresh(const SoundDocument&);
 // While the pattern unit is unfolded: the house unit's strip alone on its rails.
 void setStripOnly(bool);
 // 1547 / 2228, and 197.11 at that scale.
 float scale() const;
 float unitHeight() const;
 // Each rail number and its centre's y in the body.
 std::vector<std::pair<int,float>> railNumbers() const;
 // In stack order.
 std::vector<RackStackUnit*> units() const;
 RackStackUnit* unit(juce::Uuid) const;
 // Another rack's drawer closed, the drawer bound to the rack, onSelectionChanged (null for a generic rack), the rack
 // revealed; null selects none.
 void select(juce::Uuid);
 juce::Uuid selected() const;
 // The drawer under its rack, selecting it; one at a time.
 void openDrawer(juce::Uuid);
 void closeDrawer();
 juce::Uuid drawerRack() const;
 // The open drawer's all-controls panel, over its rack's units, above the form; scaled with the column when it is
 // narrower than the stage. Closing the drawer empties it, which ends a held knob's gesture.
 studio::AllControlsPanel& controls();
 // The rack, with its open drawer, brought into view: to its top when it is taller than the view or above it, else to
 // its foot.
 void reveal(juce::Uuid);
 // Each unit's row panel defers the editor's destruction while one of its callbacks runs; true if any does.
 bool deferEditorDestruction(std::shared_ptr<void>);
 // End graph monitoring and held gestures synchronously before editor retirement.
 void retireGraphs();
 void refreshAnalysis();
 void openEqOverview(std::uint8_t);
 void closeEqOverview();
 void openEqMatch(std::uint8_t);
 void closeEqMatch();
 // The LIBRARY's preview, or none; nothing changes unless it differs from the one shown. A new or
 // moved placement is brought into view.
 void setPlacement(std::optional<StackPlacement>);
 // The placement's component while one shows ("rack.placement"), for the tests.
 juce::Component* placement() const;
 // The proposal's candidate: existing controls get marks; added racks get read-only ghost faceplates in its order.
 void setProposal(const SoundDocument*);
 // While a drag lasts: each line's y in the body as the stack lies without the lifted racks, and its destination, none
 // for their own place; empty otherwise.
 std::vector<std::pair<int,std::optional<RackDestination>>> dropLines() const;
 bool dragging() const;
 // Escape ends a drag.
 bool keyPressed(const juce::KeyPress&) override;
 // Narrower than the stage's 1547 px: every unit but the open one shows its strip as view state, the house unit its
 // narrow strip, and the open unit scrolls sideways.
 bool narrow() const;
 // While narrow: the house unit's 3U unit, every rack folded, the selection kept; selecting a rack, the house's triangle
 // or a column that holds the panel closes it.
 void openHouse(bool);
 bool houseOpen() const;
 // While narrow: the house unit while open, else the selected rack unless it shows folded; else null.
 juce::Component* openUnit() const;
 // "rack.stack"
 juce::Viewport& viewport();
 juce::Component& body();
 juce::ScrollBar& scrollBar();
 // "rack.sideways": the open unit's scroll bar along its foot while its plate reaches past the right rail, in the unit's
 // own px.
 juce::ScrollBar& sideways();
 void resized() override;
 std::function<void(juce::Uuid)> onSelectionChanged;
 // Sentences for the session display.
 std::function<void(const juce::String&)> onReport;
 // After the stack edited the sound: the editor refreshes.
 std::function<void()> onEdited;
 std::function<void(DrumPadTarget,juce::Component*)> onDrumSource;
 std::function<void(juce::Uuid)> onDrumSourceClose;
private:
 struct Body;
 struct Divider;
 struct Presses;
 struct Placement;
 struct ProposalGhost{juce::Uuid id;StackPlacement placement;std::unique_ptr<Placement> view;};
 struct Drag;
 struct View final : juce::Viewport {
  std::function<void()> onMoved;
  void visibleAreaChanged(const juce::Rectangle<int>&) override{if(onMoved)onMoved();}
  // A wheel turn that moves the view holds the rack's controls off the wheel for a while (WheelLatch.h).
  void mouseWheelMove(const juce::MouseEvent&,const juce::MouseWheelDetails&) override;
 };
 // fit(), then arrange(); a layout asked for while fitting waits for it.
 void layout();
 // The column's folds and the house's column for the stack's width: the drawer closed if its rack shows folded, and a
 // focus the folds took given to the house's triangle while it is open, else to the selected rack.
 void fit();
 // layout(), or with `lines` only each drop line's y measured, with the slot closed, and nothing set.
 void arrange(std::vector<int>* lines);
 // The sideways bar over the open unit's foot, with its range, or hidden.
 void syncAcross();
 // The open unit's shift moved the least that shows the area of c, to its left edge when it is wider than the column;
 // nothing unless c is in the open unit's plate.
 void keepAcross(juce::Component& c,juce::Rectangle<int> area);
 // A press on a rack: a drag candidate unless the rack is generic or the press is on a key, a control (a faceplate's
 // knobs, levers and actions too) or the triangle.
 void press(RackStackUnit&,const juce::MouseEvent&);
 // Past 6 px the drag starts; then each move opens the slot at the line nearest the copy, as each scroll does.
 void pull(const juce::MouseEvent&);
 // The release: the move to the slot's line, unless it is the racks' own place or the pointer is outside the stack.
 void drop(const juce::MouseEvent&);
 // The drawer closed, the copy taken, the racks lifted and their lines measured; the focus taken for Escape.
 void lift();
 void follow();
 // The lifted copy's top in the body: the pointer's, less where the rack was grabbed.
 int copyTop() const;
 // With no move: the lifted racks and the placement back, and the focus given back.
 void endDrag();
 // select, revealing the rack only when show.
 void choose(juce::Uuid,bool show);
 // After a fold: the drawer closed if its rack folded, then the layout.
 void settle();
 // UP or DOWN on the rack: rackStep's operation applied at once, with its sentence.
 void move(juce::Uuid,bool up);
 // The drawer refreshed from the live sound, for its rack's header at once.
 void bindDrawer();
 void updateDetector();
 // The view moved the least that shows the area: to its top when it is taller than the view or above it. Unsized, as
 // when the editor selects the first rack before it is laid out, the view stays at the stack's top.
 void keepInView(juce::Rectangle<int>);
 void syncBar();
 void globalFocusChanged(juce::Component*) override;
 void scrollBarMoved(juce::ScrollBar*,double) override;
 void timerCallback() override;
 InstrumentProcessor& processor;
 bool skinsRetired=false;
 const studio::Model& model;
 studio::HouseUnit& house;
 studio::EmptyRackSpace& space;
 RackPanelComponent& drawer;
 studio::house::Host unitHost;
 std::unique_ptr<studio::FxAnalysisClock> privateAnalysisClock;
 std::shared_ptr<studio::FxAnalysisHub> analysis;
 studio::ProgrammerLook look;
 std::unique_ptr<studio::EqOverview> overview;
 std::unique_ptr<studio::EqMatchPanel> match;
 juce::Component::SafePointer<juce::Component> matchOpener;
 juce::Point<int> matchScroll;
 juce::Component::SafePointer<juce::Component> overviewOpener;
 juce::Point<int> overviewScroll;
 std::unique_ptr<Body> content;
 std::unique_ptr<studio::AllControlsPanel> panel;
 std::unique_ptr<Divider> divider;
 std::unique_ptr<Presses> presses;
 View view;
 juce::ScrollBar bar{true},across{false};
 std::vector<std::unique_ptr<RackStackUnit>> racks;
 std::vector<StackRack> order;
 // The sound last refreshed, for the placement's spot; the placement shown and where it was laid out.
 SoundDocument shown;
 std::optional<StackPlacement> placed;
 std::unique_ptr<Placement> slot;
 std::optional<StackSpot> spot;
 // The proposal's candidate, copied only when it changes, shared with every unit.
 std::shared_ptr<const SoundDocument> proposal;
 std::vector<ProposalGhost> proposalGhosts;
 std::vector<StackRack> proposalOrder;
 // A press that may become a drag, or the drag; the drag's slot.
 std::unique_ptr<Drag> drag;
 std::unique_ptr<Placement> dragSlot;
 juce::Uuid chosen=juce::Uuid::null(),drawn=juce::Uuid::null();
 // The component the focus was last reported on, kept while no component has it.
 juce::Component::SafePointer<juce::Component> focusWas;
 // The units above the space, and the space's whole units: the rails number both.
 int stackUnits=0,spaceUnits=0;
 bool stripOnly=false,fitting=false;
};
}
