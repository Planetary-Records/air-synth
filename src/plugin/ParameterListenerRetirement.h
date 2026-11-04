#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>
#include <vector>

namespace airack
{
class InstrumentProcessor;

namespace detail
{
// JUCE's listener array must not shrink inside its current notification loop.
class ParameterListenerDispatch final
{
public:
    explicit ParameterListenerDispatch (const InstrumentProcessor& p) : processor (p), previous (current) { current = this; }
    ~ParameterListenerDispatch()
    {
        current = previous;
        auto attachments = std::move (held);
        auto finishes = std::move (pendingFinishes);
        auto owners = std::move (retiredUi);
        for (auto& finish : finishes)
            finish();
        // Finish captures and detached UI still own every callback target during listener removal.
        attachments.clear();
        finishes.clear();
        owners.clear();
    }
    ParameterListenerDispatch (const ParameterListenerDispatch&) = delete;
    ParameterListenerDispatch& operator= (const ParameterListenerDispatch&) = delete;

    static void retire (const InstrumentProcessor& processor, std::unique_ptr<juce::ParameterAttachment>& attachment)
    {
        if (! attachment)
            return;
        auto* outer = outermost (processor);
        if (outer != nullptr)
            outer->held.push_back (std::move (attachment));
        else
            attachment.reset();
    }

    static bool finishAfterDispatch (const InstrumentProcessor& processor, std::function<void()> finish)
    {
        if (auto* outer = outermost (processor))
        {
            outer->pendingFinishes.push_back (std::move (finish));
            return true;
        }
        return false;
    }

    static void retainUi (const InstrumentProcessor& processor, std::shared_ptr<void> owner)
    {
        if (auto* outer = outermost (processor))
            outer->retiredUi.push_back (std::move (owner));
    }

private:
    static ParameterListenerDispatch* outermost (const InstrumentProcessor& processor)
    {
        ParameterListenerDispatch* outer = nullptr;
        for (auto* scope = current; scope != nullptr; scope = scope->previous)
            if (&scope->processor == &processor)
                outer = scope;
        return outer;
    }
    const InstrumentProcessor& processor;
    ParameterListenerDispatch* previous;
    std::vector<std::unique_ptr<juce::ParameterAttachment>> held;
    std::vector<std::function<void()>> pendingFinishes;
    std::vector<std::shared_ptr<void>> retiredUi;
    inline static thread_local ParameterListenerDispatch* current = nullptr;
};
} // namespace detail
} // namespace airack
