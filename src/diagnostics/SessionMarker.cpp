#include "SessionMarker.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace airack::feedback
{
SessionMarker::SessionMarker(juce::File f, IsAlive a, juce::String t) : folder(std::move(f)), alive(std::move(a)), token(std::move(t)) {}

int SessionMarker::begin()
{
    int cleared = 0;
    if (! folder.isDirectory() && ! folder.createDirectory().wasOk())
        return 0;
    own = folder.getChildFile(token.replaceCharacter(':', '-') + ".running");
    for (const auto& file : folder.findChildFiles(juce::File::findFiles, false, "*.running"))
    {
        if (file == own)
            continue;
        const auto other = file.loadFileAsString().upToFirstOccurrenceOf("\n", false, false).trim();
        if (alive && alive(other))
            continue;
        if (file.deleteFile())
            ++cleared;
    }
    // A marker with our own name is a leftover of an earlier session with the same identity; it is replaced.
    own.replaceWithText(token + "\n");
    unexpected = cleared > 0;
    return cleared;
}

void SessionMarker::end()
{
    if (own != juce::File())
        own.deleteFile();
}

juce::String SessionMarker::currentProcessToken()
{
#if JUCE_WINDOWS
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto start = (juce::uint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return juce::String((juce::int64) GetCurrentProcessId()) + ":" + juce::String((juce::int64) start);
#else
    return "0:0";
#endif
}

bool SessionMarker::processIsAlive(const juce::String& token)
{
#if JUCE_WINDOWS
    const auto pid = (DWORD) token.upToFirstOccurrenceOf(":", false, false).getLargeIntValue();
    const auto start = (juce::uint64) token.fromFirstOccurrenceOf(":", false, false).getLargeIntValue();
    if (pid == 0 || start == 0)
        return false;
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;  // exists but is not ours to inspect: do not raise an alarm
    FILETIME created{}, exited{}, kernel{}, user{};
    DWORD code = 0;
    const bool ok = GetProcessTimes(handle, &created, &exited, &kernel, &user) && GetExitCodeProcess(handle, &code);
    CloseHandle(handle);
    if (! ok)
        return true;
    const auto actual = (juce::uint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return actual == start && code == STILL_ACTIVE;
#else
    juce::ignoreUnused(token);
    return true;
#endif
}

namespace
{
struct Tracker
{
    juce::CriticalSection lock;
    int instances = 0;
    std::unique_ptr<SessionMarker> marker;
    bool unexpected = false, noticePending = false;
};
Tracker& tracker() { static Tracker t; return t; }
juce::File sessionFolder()
{
#if defined(_DEBUG)
    // Keeps automated native tests and their subprocesses out of real user data, as the diagnostic log does.
    const auto test = juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT", {});
    if (test.isNotEmpty() && juce::File::isAbsolutePath(test))
        return juce::File(test).getChildFile("Sessions");
#endif
    // This computer only, beside the logs: never the roaming profile, which can follow a person to another machine.
#if JUCE_WINDOWS
    const auto base = juce::File::getSpecialLocation(juce::File::windowsLocalAppData);
#else
    const auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#endif
    return base.getChildFile("AI Rack Synth").getChildFile("Sessions");
}
}

void SessionTracker::instanceCreated()
{
    auto& t = tracker();
    const juce::ScopedLock guard(t.lock);
    if (t.instances++ > 0)
        return;
    t.marker = std::make_unique<SessionMarker>(sessionFolder(), &SessionMarker::processIsAlive, SessionMarker::currentProcessToken());
    t.marker->begin();
    if (t.marker->previousSessionEndedUnexpectedly())
        t.unexpected = t.noticePending = true;
}

void SessionTracker::instanceDestroyed()
{
    auto& t = tracker();
    const juce::ScopedLock guard(t.lock);
    if (t.instances == 0 || --t.instances > 0)
        return;
    if (t.marker != nullptr)
        t.marker->end();
    t.marker.reset();
}

bool SessionTracker::previousSessionEndedUnexpectedly()
{
    auto& t = tracker();
    const juce::ScopedLock guard(t.lock);
    return t.unexpected;
}

bool SessionTracker::takeUnexpectedEndNotice()
{
    auto& t = tracker();
    const juce::ScopedLock guard(t.lock);
    const bool pending = t.noticePending;
    t.noticePending = false;
    return pending;
}
}
