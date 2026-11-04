#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>

namespace airack::studio
{
/** A file dialog: its title and patterns, saving (with an overwrite warning) or opening, in JUCE's own dialog or the
    system's. The dialog closes, without answering, if `parent` is deleted first. */
struct FileRequest
{
    juce::String title, patterns;
    bool saving = false, osDialog = false;
    juce::File initial;
    juce::Component* parent = nullptr;
};

/** What the units take from outside: a file dialog that answers with the chosen file (none if cancelled), a clock,
    the system's file drag out of the window, and the clipboard. A drag is true if it started; `done` then runs on
    the message thread when it ends, and may outlive `source`. */
struct StudioServices
{
    std::function<void (const FileRequest&, std::function<void (const juce::File&)>)> chooseFile;
    std::function<std::int64_t()> nowMs;
    std::function<bool (const juce::StringArray& files, bool canMove, juce::Component* source, std::function<void()> done)> dragFiles;
    std::function<void (const juce::String&)> copyText;
    // One-based item index, or zero on cancellation. Optional for non-interactive clients.
    std::function<void (juce::Component*, const juce::StringArray&, std::function<void(int)>)> chooseMenu;
};

StudioServices defaultStudioServices();
} // namespace airack::studio
