#pragma once
#include <juce_core/juce_core.h>

// What a musician can send us, built entirely on this computer. Nothing here touches the network: the report is text
// that the musician copies, saves or pastes where they choose.
namespace airack::feedback
{
inline constexpr int maxMessageBytes = 4000, maxEmailBytes = 200, maxLogLines = 40, maxLogBytes = 6 * 1024;
inline constexpr int maxReportBytes = 16 * 1024;

struct Environment
{
    juce::String releaseVersion, operatingSystem, host, format;
    double sampleRate = 0.0;
    int blockSize = 0;
    bool previousSessionEndedNormally = true;
};

struct Input
{
    juce::String message, email;
    bool includeDetails = true;
};

/** "AIR-" and eight letters or digits, without the easily confused I, O, 0 and 1. */
juce::String newReportId(juce::Random&);
bool isReportId(const juce::String&);

/** Hides the user's folder name in any path (after \Users\ or /users/) and each given word, wherever it appears. */
juce::String scrub(const juce::String& text, const juce::StringArray& privateWords);

/** The newest non-empty lines of a log, at most maxLines and maxBytes together. */
juce::StringArray tailLines(const juce::String& logText, int maxLines, int maxBytes);

/** The exact text shown in the panel and copied or saved. Always at most maxReportBytes. */
juce::String buildReport(const Input&, const Environment&, const juce::String& reportId, const juce::StringArray& logLines);

/** The page where the report goes. Only the report ID and the version are in the address. */
juce::URL feedbackPageUrl(const juce::String& reportId, const juce::String& releaseVersion);
}
