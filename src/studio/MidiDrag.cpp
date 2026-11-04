#include "MidiDrag.h"
#include "PatternIO.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace airack::studio
{
namespace
{
/** A read handle that admits other readers and denies writers and deletion. */
struct ReadLease
{
    HANDLE handle = INVALID_HANDLE_VALUE;

    explicit ReadLease (const juce::File& file)
        : handle (CreateFileW (file.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }
    ~ReadLease()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle (handle);
    }
    ReadLease (const ReadLease&) = delete;
    ReadLease& operator= (const ReadLease&) = delete;

    bool opened() const { return handle != INVALID_HANDLE_VALUE; }
};

bool ours (const juce::File& file)
{
    const auto id = file.getFileNameWithoutExtension().substring (7);
    return ! file.isSymbolicLink() && id.length() == 32 && id.containsOnly ("0123456789abcdef");
}
} // namespace

MidiDragStart dragPatternMidi (const PatternDocument& pattern, juce::Component& source, const StudioServices& services,
                               std::function<void()> ended)
{
    if (! validatePatternDocument (pattern) || pattern.noteCount == 0)
        return { "MIDI drag failed: pattern is empty or invalid; add a note first" };
    // Instances and processes share the folder; only our own names are ever removed, never links or other files.
    juce::InterProcessLock lock ("AI Rack Synth MIDI exports v1");
    if (! lock.enter (0))
        return { "MIDI drag storage is busy; retry the drag" };
    struct Unlock
    {
        juce::InterProcessLock& lock;
        ~Unlock() { lock.exit(); }
    } unlock { lock };
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("AI Rack Synth MIDI");
    if (folder.isSymbolicLink() || ! folder.createDirectory())
        return { "MIDI drag failed: cannot create temporary export folder" };
    const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::hours (24);
    int retained = 0;
    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false, "airack-*.mid"))
    {
        if (! ours (file))
            continue;
        if (file.getLastModificationTime() < cutoff)
            file.deleteFile();
        if (file.existsAsFile())
            ++retained;
    }
    if (retained >= 128)
        return { "MIDI drag storage full: older exports must expire before another drag" };
    const auto file = folder.getChildFile ("airack-" + juce::Uuid().toString() + ".mid");
    if (file.exists() || ! exportPatternMidi (pattern, file))
        return { "MIDI drag failed: cannot write temporary MIDI file" };
    auto lease = std::make_shared<ReadLease> (file);
    if (! lease->opened())
    {
        lease.reset();
        file.deleteFile();
        return { "MIDI drag failed: cannot retain temporary MIDI file" };
    }
    const bool started = services.dragFiles != nullptr
                         && services.dragFiles ({ file.getFullPathName() }, false, &source, [lease, done = std::move (ended)]
                                                {
                                                    if (done)
                                                        done();
                                                });
    if (! started)
    {
        lease.reset();
        file.deleteFile();
        return { "MIDI drag failed: native file drag could not start" };
    }
    return { "MIDI file ready: drop into your DAW to copy the pattern", true };
}
} // namespace airack::studio
