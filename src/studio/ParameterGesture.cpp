#include "ParameterGesture.h"

#include "HouseControls.h"
#include "InstrumentProcessor.h"
#include "ParameterListenerRetirement.h"

namespace airack::studio
{
// Calls and deferred retirement can outlive the control that started them.
struct ParameterGesture::State
{
    State (InstrumentProcessor& p, std::size_t i, const house::Host& h, std::function<void (float)> read)
        : processor (p), index (i), host (h), show (std::move (read)) {}
    bool closing() const
    {
        if (! attached) return true;
        auto callback = host.closing;
        return (callback && callback()) || ! attached;
    }
    InstrumentProcessor& processor;
    const std::size_t index;
    const house::Host& host;
    std::function<void (float)> show;
    unsigned calls = 0;
    bool attached = true, retiring = false, dragging = false;
};

struct ParameterGesture::Call
{
    std::shared_ptr<State> state;
    std::shared_ptr<void> owner;
    detail::ParameterListenerDispatch dispatch;
    explicit Call (std::shared_ptr<State> s) : state (std::move (s)), dispatch (state->processor)
    {
        ++state->calls;
        if (state->attached) { auto frame = state->host.frame; if (frame) owner = frame(); }
    }
    ~Call()
    {
        if (--state->calls == 0 && state->retiring)
        {
            state->retiring = false;
            finish (state);
        }
    }
};

ParameterGesture::ParameterGesture (InstrumentProcessor& p, std::size_t parameter, const house::Host& h, std::function<void (float)> read)
    : index (parameter), state (std::make_shared<State> (p, parameter, h, std::move (read))) {}

ParameterGesture::~ParameterGesture()
{
    auto s = state;
    s->attached = false;
    retire (s);
}

bool ParameterGesture::held() const { return state->dragging; }
void ParameterGesture::retire() { auto s = state; retire (s); }
void ParameterGesture::retire (const std::shared_ptr<State>& s)
{
    if (s->calls != 0) s->retiring = true;
    else if (s->dragging && ! detail::ParameterListenerDispatch::finishAfterDispatch (s->processor, [s] { finish (s); })) finish (s);
}

bool ParameterGesture::begin()
{
    auto s = state;
    const Call scope (s);
    if (! s->dragging && ! s->closing())
    {
        s->dragging = s->processor.beginParameterGesture (s->index);
        if (s->closing() && s->dragging)
            finish (s);
        else if (! s->dragging)
            reject (s);
    }
    return s->dragging && ! s->retiring;
}

void ParameterGesture::write (float physical)
{
    auto s = state;
    const Call scope (s);
    if (s->closing()) return;
    const bool discrete = ! s->dragging;
    if (discrete && ! s->processor.beginParameterGesture (s->index))
    {
        reject (s);
        return;
    }
    const bool ok = ! s->closing() && s->processor.setParameterGestureValue (s->index, s->processor.control (s->index).convertTo0to1 (physical));
    const bool ended = ! discrete || s->processor.endParameterGesture (s->index);
    if (s->closing() && s->dragging)
        finish (s);
    else if ((! ok || ! ended) && ! s->closing())
        reject (s);
}

void ParameterGesture::end() { auto s = state; finish (s); }
void ParameterGesture::finish (const std::shared_ptr<State>& s)
{
    if (s->calls != 0) { s->retiring = true; return; }
    if (! s->dragging) return;
    const Call scope (s);
    if (s->dragging)
    {
        s->dragging = false;
        const auto ok = s->processor.endParameterGesture (s->index);
        if (! ok && ! s->closing()) reject (s);
    }
}

void ParameterGesture::reject() { auto s = state; reject (s); }
void ParameterGesture::reject (const std::shared_ptr<State>& s)
{
    if (s->closing()) return;
    auto& param = s->processor.control (s->index);
    auto show = s->show;
    if (show) show (param.convertFrom0to1 (param.getValue()));
    if (! s->attached) return;
    auto report = s->host.report;
    if (report) report (rejectedEdit);
}
} // namespace airack::studio
