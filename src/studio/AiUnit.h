#pragma once

#include "InstrumentProcessor.h"
#include "SkinView.h"
#include "StudioServices.h"

#include <optional>
#include <vector>

namespace airack::studio
{
/** The AI unit over the processor: ACCOUNT and the model display, LINKED INSTANCES, MCP, the screen's pages and
    GENERATE to DISCARD, with today's AI and group panels' behaviour. MCP is lit while agents can reach this synth,
    and its page turns their access on and off. GENERATE and REFINE blink while their request runs,
    AUDITION is lit while the proposal plays, and APPLY blinks while an Apply waits, else is lit while a proposal with
    audio is ready. Today's
    sentences stay in the undrawn ai.status and group.status and the read-only ai.diff; the screen shows them
    shortened, and a press that cannot act says why on its status line. It asks the companion nothing until it first
    shows, and going closes the AI panel as today's did. */
class AiUnit final : public juce::Component, private juce::Timer
{
public:
    AiUnit (InstrumentProcessor& p, const Model& model, StudioServices s);
    ~AiUnit() override;
    /** Release processor callbacks before deferred editor deletion. */
    void retire();

    juce::String sentence() const { return status.getText(); }

    void visibilityChanged() override;

private:
    /** What the presses and the pages are decided from, as today's refresh read it. */
    struct Facts
    {
        AiWorkflowStatus workflow;
        const companion::AccountReply* account = nullptr;
        GroupWorkflowView group;
        AiProposalPhase phase = AiProposalPhase::idle;
        const ai::GenerationResult* result = nullptr;
        const ai::GenerationRequest* request = nullptr;
        /** The account's last reply is signed in; `connected` also needs the client ready and no account request. */
        bool signedIn = false, connected = false, selected = false, proposed = false, eligible = false;
        bool actualGrouped = false, refinable = true, current = true, audioReady = false;
        bool stopped = true, playing = false;
        /** The proposal's name, empty for an answer without one. */
        juce::String name;
    };
    /** What a press's message stands for: it goes when the request, the proposal or its phase changes. */
    struct Stamp
    {
        std::uint64_t request = 0;
        const void* proposal = nullptr;
        AiProposalPhase phase = AiProposalPhase::idle;
        bool operator== (const Stamp&) const = default;
    };
    struct Choice { GroupUuid id; juce::String name; };

    void refresh();
    void timerCallback() override { refresh(); }
    Facts facts() const;
    Stamp stampNow() const;
    void refuse (const juce::String& reason);
    juce::Button* button (const char* id) const;

    void toggleAccount();
    void toggleLinked();
    void toggleMcp();
    void command (const juce::String& command, int value);
    void entered (const juce::String& command, const juce::String& text);
    void generate();
    void refine();
    void audition();
    void stop();
    void apply();
    void discard();
    void cancel();
    void accountButton();
    void showAccounts();
    void chooseAccount(const juce::String& id);
    void addAccount();
    void chooseModel (int row);
    void joinGroup (int row);

    void updateContext();
    juce::String todaysSentence (const Facts& f) const;
    juce::String proposalDifference (std::vector<ChangeRow>& rows) const;
    juce::String groupSentence (const GroupWorkflowView& view, std::vector<PeerRow>& rows);
    void link (GroupUuid group, const juce::String& groupName);
    void selectPeer (int row);
    ScreenShown shownFor (const Facts& f, const McpView& mcp, const juce::String& sentence, const juce::String& groupText,
                          const juce::String& details, std::vector<ChangeRow> changes, std::vector<PeerRow> peers) const;
    juce::String pageFor (const Facts& f) const;

    InstrumentProcessor& processor;
    StudioServices services;
    SkinView view;
    juce::Label status, groupStatus;
    juce::TextEditor difference;

    // today's AI panel's fields, options and selection
    juce::String prompt, role, key, scale, tempo;
    int scope = 0;
    bool requirePattern = false, macros = false, reuse = false;
    bool contextValid = true;
    juce::String contextError;
    std::vector<juce::String> modelSlugs, modelNames;
    int chosenModel = -1;
    // today's group panel's
    std::vector<Choice> choices;
    std::vector<GroupUuid> peerMembers;
    juce::String actionMessage, groupName;
    // the unit's own: the page asked for (compose, account, linked, join or mcp), the page shown, and a press's message
    juce::String page = "compose", shownPage;
    juce::String pressMessage;
    Stamp pressStamp;
    struct Leds { bool generate = false, refine = false, audition = false, apply = false; } leds;
    bool signingIn = false, retired = false;
    // The MCP light, which the plate lacks: LINKED INSTANCES' element moved left of it, its print, and the light.
    Element mcpElement;
    std::unique_ptr<juce::Component> mcpPrint;
    std::unique_ptr<Target> mcpLight;
};
} // namespace airack::studio
