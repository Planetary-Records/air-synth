#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace airack
{
class InstrumentProcessor;
}

namespace airack::studio
{
namespace house
{
struct Host;
}

/** What the host says when the processor refuses a control's edit. */
inline constexpr const char* rejectedEdit = "Edit rejected; showing current sound (a recall or another gesture may have won)";

/** One parameter's edits from a control, by the rules today's controls keep. A drag is one gesture, from begin() to
    end(); outside a drag each write() is one complete gesture. The processor refuses an edit while another gesture or
    an AI reservation holds the parameter: then the control is shown the parameter's value again and the host reports
    why. Each call holds the host's frame, and nothing is written or reported while the host closes. */
class ParameterGesture
{
public:
    /** `read` shows the control a physical value. */
    ParameterGesture (InstrumentProcessor& p, std::size_t parameter, const house::Host& h, std::function<void (float)> read);

    ~ParameterGesture();
    ParameterGesture (const ParameterGesture&) = delete;
    ParameterGesture& operator= (const ParameterGesture&) = delete;

    /** A drag starts; false when the processor refuses it, which is reported. */
    bool begin();
    void write (float physical);
    /** The drag ends, if one is held. */
    void end();
    /** Finish an idle drag now, or after its active host notification returns. */
    void retire();
    /** Shows the control the parameter's value and says why its edit was refused. */
    void reject();
    bool held() const;

    const std::size_t index;

private:
    struct State;
    struct Call;
    static void finish (const std::shared_ptr<State>&);
    static void retire (const std::shared_ptr<State>&);
    static void reject (const std::shared_ptr<State>&);
    std::shared_ptr<State> state;
};
} // namespace airack::studio
