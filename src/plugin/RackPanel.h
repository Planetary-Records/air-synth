#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "SoundDocument.h"
#include <optional>
#include <functional>
namespace airack::studio { struct Model; }
namespace airack {
class InstrumentProcessor;
// The selected rack's controls and its operations, drawn in the PROGRAMMER's style from the skin's parts.
class RackPanelComponent final : public juce::Component {
public:
 RackPanelComponent(InstrumentProcessor&,const studio::Model&);
 ~RackPanelComponent() override;
 // In the drawer a generic rack's ID (bareBankId) shows its bank's header and no operation.
 void showInstance(juce::Uuid);
 // The status line keeps the LIBRARY's Apply sentence, as it kept the library place's.
 void showStatus(const juce::String&);
 void refresh(const SoundDocument&);
 // Both editor views enter the same callback guard before host notifications.
 // Retire the whole editor only after every begin/write/end frame unwinds.
 std::shared_ptr<void> editorCallbackFrame();
 bool deferEditorDestruction(std::shared_ptr<void>,std::function<void()> detach={});
 // Out of the editor and deleted on a later turn: it reads the processor no more from now, as its destruction does, since
 // the editor and the processor may go before that turn.
 void retire();
 void resized() override;
 // A standalone panel keeps the whole form. In a rack unit the panel is the rack's row alone: no list, header, ADVANCED
 // or form, no face and no scrolling, the row centred in the panel's height. As the stack's drawer it is the selected
 // rack's header lines, the operations on that rack and ADVANCED, with no list and no row; the LIBRARY offers the ways
 // to add one. As a preview it lays the row out as a rack unit does, from a design it is shown, bound to no parameter and
 // inert. Changing place cancels the draft.
 enum class Place{all,unit,drawer,preview};
 void setPlace(Place);
 Place place() const;
 // In Place::preview: the rack's row from the rack and its normalized values, one per control, bound to no parameter.
 void showDesign(const RackInstance&,std::vector<float> values);
 // In Place::unit: the proposal's candidate sound, or none. A control is marked when the candidate keeps the rack with
 // the same sections, controls and bindings, the control is not locked, and its value differs from the live one; a
 // rack the candidate keeps with another panel or other bindings has the row's outline instead.
 void showProposal(std::shared_ptr<const SoundDocument>);
 struct Mark{std::size_t control=0;float proposed=0;bool operator==(const Mark&) const=default;};
 // The marked controls and their proposed physical values, in panel order.
 std::vector<Mark> marks() const;
 bool proposesPanel() const;
 // In Place::unit: each row control's parameter, in panel order.
 std::vector<std::size_t> parameters() const;
 std::function<void()> onAdvanced;
 std::function<void(juce::Uuid)> onSelectionChanged;
 // In the drawer, after an APPLY with a sentence: the sentence and the rack it placed, if any.
 std::function<void(const juce::String&,std::optional<juce::Uuid>)> onApplied;
 // The drawer's height: from its top to 20 px under the difference box, or under the status line while the box is empty
 // and hidden.
 int drawerHeight() const;
 bool detailsCollapsed() const;
 // The saturation service cover controls only the existing local form, without changing sound or undo history.
 void setDetailsOpen(bool);
 std::function<void(bool)> onDetailsChanged;
 bool keyPressed(const juce::KeyPress&) override;
 // In the drawer, after the difference box shows or hides.
 std::function<void()> onHeight;
 // In the drawer: the plate's face over this area, in the drawer's own pixels, so that it continues the all-controls
 // panel's; the drawer's bounds while it is empty.
 void setFace(juce::Rectangle<float>);
private:
 struct Content;
 std::shared_ptr<Content> content;
};
}

