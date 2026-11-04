// The report a musician can send, and the marker that notices a session that did not end normally.
// Pure logic: no window, no network, no host.
#include "FeedbackReport.h"
#include "SessionMarker.h"

#include <juce_core/juce_core.h>
#include <iostream>
#include <cstdlib>
#include <regex>
#include <set>

namespace
{
using namespace airack::feedback;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cout << "FAIL " << __LINE__ << ": " << #x << '\n'; } } while (false)

Environment sampleEnvironment()
{
    Environment e;
    e.releaseVersion = "0.1.0-alpha.1";
    e.operatingSystem = "Windows 11";
    e.host = "Reaper";
    e.format = "VST3";
    e.sampleRate = 48000.0;
    e.blockSize = 512;
    e.previousSessionEndedNormally = true;
    return e;
}

void reportIds()
{
    std::cout << "SECTION report_ids\n";
    juce::Random random(7);
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i)
    {
        const auto id = newReportId(random);
        CHECK(isReportId(id));
        CHECK(std::regex_match(id.toStdString(), std::regex("AIR-[A-HJ-NP-Z2-9]{8}")));
        seen.insert(id.toStdString());
    }
    CHECK(seen.size() == 200);
    for (const char* bad : { "", "AIR-", "AIR-ABCDEFG", "AIR-ABCDEFGHI", "air-ABCDEFGH", "AIR-ABCDEFG0", "AIR-ABCDEFGI", "XXX-ABCDEFGH" })
        CHECK(!isReportId(bad));
}

void reportContent()
{
    std::cout << "SECTION report_content\n";
    const auto id = juce::String("AIR-ABCD2345");
    Input in;
    in.message = "It goes quiet after I save.";
    in.email = "";
    in.includeDetails = true;
    const auto full = buildReport(in, sampleEnvironment(), id, { "11:02 plugin startup ok", "11:03 companion launch ok" });
    CHECK(full.contains("Report ID: AIR-ABCD2345"));
    CHECK(full.contains("Version: 0.1.0-alpha.1"));
    CHECK(full.contains("It goes quiet after I save."));
    CHECK(full.contains("Windows 11") && full.contains("Reaper") && full.contains("VST3"));
    CHECK(full.contains("48000") && full.contains("512"));
    CHECK(full.contains("11:03 companion launch ok"));
    CHECK(!full.contains("Email:"));

    in.email = "me@example.com";
    CHECK(buildReport(in, sampleEnvironment(), id, {}).contains("Email: me@example.com"));

    // Technical details are optional; without them only the message, version and ID remain.
    in.includeDetails = false;
    const auto plain = buildReport(in, sampleEnvironment(), id, { "11:02 plugin startup ok" });
    CHECK(plain.contains("Report ID: AIR-ABCD2345") && plain.contains("Version: 0.1.0-alpha.1") && plain.contains("It goes quiet"));
    CHECK(!plain.contains("Windows") && !plain.contains("Reaper") && !plain.contains("48000") && !plain.contains("plugin startup"));

    // A session that did not end normally is said so, only with details.
    auto crashed = sampleEnvironment();
    crashed.previousSessionEndedNormally = false;
    in.includeDetails = true;
    CHECK(buildReport(in, crashed, id, {}).contains("Last session ended normally: no"));
    CHECK(buildReport(in, sampleEnvironment(), id, {}).contains("Last session ended normally: yes"));
}

void privacy()
{
    std::cout << "SECTION privacy\n";
    const juce::StringArray privateWords{ "test-producer", "STUDIO-PC" };
    const auto cleaned = scrub("C:\\Users\\test-producer\\AppData\\Local\\AI Rack Synth\\Logs\\a.log by test-producer on studio-pc, d:/users/Someone Else/x", privateWords);
    CHECK(!cleaned.containsIgnoreCase("test-producer") && !cleaned.containsIgnoreCase("studio-pc") && !cleaned.containsIgnoreCase("Someone"));
    CHECK(cleaned.contains("C:\\Users\\<user>\\AppData\\Local\\AI Rack Synth\\Logs\\a.log"));
    CHECK(cleaned.contains("d:/users/<user>/x"));
    CHECK(scrub("nothing private here", privateWords) == "nothing private here");
    CHECK(scrub("keep ab", { "", "a" }) == "keep <private>b");  // an empty word is ignored, a short one is not
    // The same scrubbing runs on the technical details of every report.
    Input in;
    in.message = "hello";
    const auto report = buildReport(in, sampleEnvironment(), "AIR-ABCD2345", { scrub("log C:\\Users\\test-producer\\x", privateWords) });
    CHECK(!report.contains("test-producer"));
}

void bounds()
{
    std::cout << "SECTION bounds\n";
    juce::String log;
    for (int i = 0; i < 5000; ++i)
        log << "line " << i << " " << juce::String::repeatedString("x", 120) << "\n";
    const auto tail = tailLines(log, 40, 6 * 1024);
    CHECK(tail.size() <= 40);
    int bytes = 0;
    for (const auto& l : tail) bytes += l.getNumBytesAsUTF8();
    CHECK(bytes <= 6 * 1024);
    CHECK(tail.size() > 0 && tail[tail.size() - 1].startsWith("line 4999"));
    CHECK(tailLines("", 40, 1024).isEmpty());
    CHECK(tailLines("a\r\nb\r\n\r\nc", 10, 1024) == juce::StringArray({ "a", "b", "c" }));

    Input in;
    in.message = juce::String::repeatedString("m", 100000);
    in.email = juce::String::repeatedString("e", 100000);
    const auto report = buildReport(in, sampleEnvironment(), "AIR-ABCD2345", tail);
    CHECK(report.getNumBytesAsUTF8() <= (size_t) maxReportBytes);
    CHECK(report.contains("Report ID: AIR-ABCD2345"));
    CHECK(report.contains("(shortened)"));
}

void pageUrl()
{
    std::cout << "SECTION page_url\n";
    const auto url = feedbackPageUrl("AIR-ABCD2345", "0.1.0-alpha.1").toString(true);
    CHECK(url.startsWith("https://air.planetary.music/feedback"));
    CHECK(url.contains("report=AIR-ABCD2345") && url.contains("version=0.1.0-alpha.1"));
    // Only the ID and the version travel; never the message, the email or the details.
    CHECK(!url.contains("@") && !url.contains("message") && !url.contains("email"));
}

void sessionMarker()
{
    std::cout << "SECTION session_marker\n";
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("air-session-" + juce::Uuid().toString());
    CHECK(dir.createDirectory());
    std::set<juce::String> alive{ "100:1", "200:2" };
    const auto isAlive = [&](const juce::String& token) { return alive.count(token) != 0; };

    {
        SessionMarker first(dir, isAlive, "100:1");
        CHECK(first.begin() == 0 && !first.previousSessionEndedUnexpectedly());
        CHECK(dir.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 1);
        // A second live process beside it is not an unexpected end.
        SessionMarker other(dir, isAlive, "200:2");
        CHECK(other.begin() == 0 && !other.previousSessionEndedUnexpectedly());
        CHECK(dir.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 2);
        other.end();
        CHECK(dir.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 1);
        // Ending twice is harmless.
        other.end();
        first.end();
    }
    CHECK(dir.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 0);

    // A marker left by a process that is gone: the next session notices it once, and removes it.
    {
        SessionMarker crashed(dir, isAlive, "300:3");
        CHECK(crashed.begin() == 0);
        // The process dies without calling end().
    }
    CHECK(dir.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 1);
    {
        SessionMarker next(dir, isAlive, "100:1");
        CHECK(next.begin() == 1 && next.previousSessionEndedUnexpectedly());
        CHECK(dir.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 1);  // only its own
        next.end();
    }
    {
        SessionMarker later(dir, isAlive, "100:1");
        CHECK(later.begin() == 0 && !later.previousSessionEndedUnexpectedly());
        later.end();
    }
    // Unrelated files and an unreadable folder never raise an alarm or throw.
    dir.getChildFile("notes.txt").replaceWithText("x");
    {
        SessionMarker quiet(dir, isAlive, "100:1");
        CHECK(quiet.begin() == 0 && !quiet.previousSessionEndedUnexpectedly());
        quiet.end();
    }
    SessionMarker nowhere(dir.getChildFile("missing").getChildFile("deeper"), isAlive, "100:1");
    CHECK(nowhere.begin() == 0 && !nowhere.previousSessionEndedUnexpectedly());
    nowhere.end();
    // The real process token names this process and is stable within it.
    CHECK(SessionMarker::currentProcessToken().isNotEmpty() && SessionMarker::currentProcessToken() == SessionMarker::currentProcessToken());
    dir.deleteRecursively();
}
}

#if defined(_WIN32) && defined(_DEBUG)
// The plug-in's own use of the marker: one per process, a stale one noticed once. Debug builds keep it in a private folder.
void sessionTracker()
{
    std::cout << "SECTION session_tracker\n";
    auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("air-tracker-" + juce::Uuid().toString());
    const auto sessions = root.getChildFile("Sessions");
    CHECK(sessions.createDirectory());
    // A marker left by a process that no longer exists (process 1 is never a running process of ours).
    CHECK(sessions.getChildFile("1-1.running").replaceWithText("1:1\n"));
    _putenv_s("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT", root.getFullPathName().toRawUTF8());

    CHECK(!SessionTracker::previousSessionEndedUnexpectedly() && !SessionTracker::takeUnexpectedEndNotice());
    SessionTracker::instanceCreated();
    CHECK(SessionTracker::previousSessionEndedUnexpectedly());
    CHECK(!sessions.getChildFile("1-1.running").exists());                                   // cleared once
    CHECK(sessions.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 1);          // this process's own
    CHECK(SessionTracker::takeUnexpectedEndNotice());                                        // offered once
    CHECK(!SessionTracker::takeUnexpectedEndNotice());
    SessionTracker::instanceCreated();                                                       // a second instance in the same process
    CHECK(sessions.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 1);
    SessionTracker::instanceDestroyed();
    CHECK(sessions.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 1);          // one instance still open
    SessionTracker::instanceDestroyed();
    CHECK(sessions.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 0);          // a normal end leaves nothing
    SessionTracker::instanceDestroyed();                                                     // extra calls are harmless
    CHECK(sessions.getNumberOfChildFiles(juce::File::findFiles, "*.running") == 0);
    _putenv_s("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT", "");
    root.deleteRecursively();
}
#endif

int main()
{
    reportIds();
    reportContent();
    privacy();
    bounds();
    pageUrl();
    sessionMarker();
#if defined(_WIN32) && defined(_DEBUG)
    sessionTracker();
#endif
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
