#include "FeedbackReport.h"

namespace airack::feedback
{
namespace
{
constexpr const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int idLength = 8;

// Whole characters only, so a cut never splits one.
juce::String shortened(const juce::String& text, int maxBytes, bool& wasCut)
{
    if ((int) text.getNumBytesAsUTF8() <= maxBytes)
        return text;
    wasCut = true;
    auto result = text;
    while (result.isNotEmpty() && (int) result.getNumBytesAsUTF8() > maxBytes)
        result = result.dropLastCharacters(juce::jmax(1, result.length() / 10));
    return result;
}

// Backslash is character 92; kept numeric so no escape is needed.
bool isSeparator(juce::juce_wchar c) { return c == 92 || c == '/'; }

int bytes(const juce::String& s) { return (int) s.getNumBytesAsUTF8(); }
}

juce::String newReportId(juce::Random& random)
{
    juce::String id("AIR-");
    for (int i = 0; i < idLength; ++i)
        id << alphabet[random.nextInt(32)];
    return id;
}

bool isReportId(const juce::String& text)
{
    if (text.length() != 4 + idLength || ! text.startsWith("AIR-"))
        return false;
    for (int i = 4; i < text.length(); ++i)
        if (juce::String(alphabet).indexOfChar(text[i]) < 0)
            return false;
    return true;
}

juce::String scrub(const juce::String& text, const juce::StringArray& privateWords)
{
    // Folders first: whatever follows \Users\ or /users/ up to the next separator is a person's name.
    juce::String pathsHidden;
    for (int i = 0; i < text.length();)
    {
        pathsHidden << text[i];
        const auto here = text.substring(i);
        const bool marker = here.length() > 7 && isSeparator(here[0]) && here.substring(1, 6).equalsIgnoreCase("users")
                            && isSeparator(here[6]);
        if (marker)
        {
            pathsHidden << here.substring(1, 7) << "<user>";
            int end = 7;
            while (end < here.length() && ! isSeparator(here[end]))
                ++end;
            i += end;
            continue;
        }
        ++i;
    }
    // Then each private word, in one pass over the text so a replacement is never scrubbed again.
    juce::StringArray words;
    for (const auto& w : privateWords)
        if (w.isNotEmpty())
            words.add(w);
    if (words.isEmpty())
        return pathsHidden;
    juce::String result;
    for (int i = 0; i < pathsHidden.length();)
    {
        int matched = 0;
        for (const auto& w : words)
            if (w.length() > matched && pathsHidden.substring(i, i + w.length()).equalsIgnoreCase(w))
                matched = w.length();
        if (matched > 0) { result << "<private>"; i += matched; }
        else { result << pathsHidden[i]; ++i; }
    }
    return result;
}

juce::StringArray tailLines(const juce::String& logText, int maxLines, int maxBytes)
{
    juce::StringArray all;
    all.addLines(logText);
    juce::StringArray kept;
    int total = 0;
    for (int i = all.size() - 1; i >= 0 && kept.size() < maxLines; --i)
    {
        auto line = all[i].trim();
        if (line.isEmpty())
            continue;
        line = line.substring(0, 500);
        if (total + bytes(line) > maxBytes)
            break;
        total += bytes(line);
        kept.insert(0, line);
    }
    return kept;
}

juce::String buildReport(const Input& in, const Environment& env, const juce::String& reportId, const juce::StringArray& logLines)
{
    bool cut = false;
    const auto message = shortened(in.message.trim(), maxMessageBytes, cut);
    const auto email = shortened(in.email.trim(), maxEmailBytes, cut);

    juce::String text;
    text << "air feedback report\n"
         << "Report ID: " << reportId << "\n"
         << "Version: " << env.releaseVersion << "\n";
    if (email.isNotEmpty())
        text << "Email: " << email << "\n";
    text << "\nWhat happened:\n" << message << "\n";
    if (cut)
        text << "(shortened)\n";

    if (in.includeDetails)
    {
        text << "\nTechnical details (included because the box is ticked):\n"
             << "Windows: " << env.operatingSystem << "\n"
             << "Host: " << env.host << "\n"
             << "Format: " << env.format << "\n"
             << "Audio: " << juce::String(env.sampleRate, 0) << " Hz, " << env.blockSize << " samples\n"
             << "Last session ended normally: " << (env.previousSessionEndedNormally ? "yes" : "no") << "\n";
        if (! logLines.isEmpty())
        {
            text << "\nRecent log lines:\n";
            // Newest lines win if they would not all fit.
            juce::StringArray fit;
            int room = maxLogBytes;
            for (int i = logLines.size() - 1; i >= 0 && fit.size() < maxLogLines; --i)
            {
                const auto line = logLines[i].substring(0, 500);
                if (bytes(line) + 1 > room)
                    break;
                room -= bytes(line) + 1;
                fit.insert(0, line);
            }
            for (const auto& line : fit)
                text << line << "\n";
        }
    }
    jassert(bytes(text) <= maxReportBytes);
    return text;
}

juce::URL feedbackPageUrl(const juce::String& reportId, const juce::String& releaseVersion)
{
    return juce::URL("https://air.planetary.music/feedback").withParameter("report", reportId).withParameter("version", releaseVersion);
}
}
