#pragma once
#include <juce_core/juce_core.h>
#include <functional>

// Notices a session that did not end normally, without any network. A running session leaves a small marker file; a
// normal end removes it. A marker whose process is gone means the last session ended unexpectedly (the music program or
// air crashed, or the computer lost power); it is reported once and removed.
namespace airack::feedback
{
class SessionMarker
{
public:
    using IsAlive = std::function<bool(const juce::String& token)>;

    SessionMarker(juce::File folder, IsAlive alive, juce::String token);

    /** Writes this session's marker and clears markers left by sessions that are gone. Returns how many it cleared.
        Best effort: a folder that cannot be used means no marker and no alarm. */
    int begin();
    /** Removes this session's marker. Harmless to repeat. A destructor does not end a session. */
    void end();
    bool previousSessionEndedUnexpectedly() const { return unexpected; }

    /** "<process id>:<start time>", which names this process even if its id is reused later. */
    static juce::String currentProcessToken();
    static bool processIsAlive(const juce::String& token);

private:
    juce::File folder, own;
    IsAlive alive;
    juce::String token;
    bool unexpected = false;
};

/** The plug-in's use of the marker: one per host process, from the first instance to the last. */
class SessionTracker
{
public:
    static void instanceCreated();
    static void instanceDestroyed();
    /** True when the session before this one ended unexpectedly (for the whole life of this process). */
    static bool previousSessionEndedUnexpectedly();
    /** True once, the first time it is asked after such a session, so the prompt appears once per process. */
    static bool takeUnexpectedEndNotice();
};
}
