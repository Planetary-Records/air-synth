#include "AiUnit.h"
#include "RackOrder.h"


#include <algorithm>
#include <charconv>
#include <cmath>

namespace airack::studio
{
namespace
{
void identify (juce::Component& c, const juce::String& id, const juce::String& name)
{
    c.setComponentID (id);
    c.setName (name);
}

juce::String ownerName (std::uint8_t chain)
{
    return chain == masterChain ? juce::String ("Master") : "Synth " + juce::String (chain + 1) + " insert";
}

juce::String noteText (const PatternNote& note)
{
    return "pitch " + juce::String (note.pitch) + ", beat " + juce::String (note.startBeats, 3) + ", duration " + juce::String (note.durationBeats, 3)
           + ", velocity " + juce::String (note.velocity) + ", channel " + juce::String (note.channel);
}

juce::String text (const GroupText& value)
{
    const auto s = groupTextView (value);
    return juce::String::fromUTF8 (s.data(), int (s.size()));
}

juce::String uuid (GroupUuid value) { return juce::Uuid (value.getRawData()).toString(); }

juce::String availability (ObservedMidiAvailability value)
{
    return value == ObservedMidiAvailability::fresh ? "fresh" : value == ObservedMidiAvailability::stale ? "stale" : "unavailable";
}

juce::String noteReason (ObservedMidiReason value)
{
    switch (value)
    {
        case ObservedMidiReason::none: return {};
        case ObservedMidiReason::unprepared: return "not prepared";
        case ObservedMidiReason::internalMode: return "Internal pattern mode";
        case ObservedMidiReason::missingTiming: return "missing host timing";
        case ObservedMidiReason::invalidTiming: return "invalid host timing";
        case ObservedMidiReason::invalidBlock: return "invalid audio block";
        case ObservedMidiReason::inputCapacity: return "input capacity exceeded";
        case ObservedMidiReason::stopped: return "stopped";
        case ObservedMidiReason::noNotes: return "no observed notes";
        case ObservedMidiReason::stalePublisher: return "stale publisher";
        case ObservedMidiReason::clockRollback: return "clock rollback";
    }
    return "unavailable";
}

juce::String controlReason (LiveControlReason value)
{
    switch (value)
    {
        case LiveControlReason::none: return {};
        case LiveControlReason::notCaptured: return "not captured";
        case LiveControlReason::readConflict: return "read conflict";
        case LiveControlReason::invalidRouting: return "invalid routing";
        case LiveControlReason::stalePublisher: return "stale publisher";
        case LiveControlReason::clockRollback: return "clock rollback";
    }
    return "unavailable";
}

juce::String groupWait (const GroupWorkflowView& group)
{
    return group.conflicted ? "Resolve conflicting group settings before Generate"
         : group.busy       ? "Finish the current edit before Generate"
                            : "Waiting for group acknowledgment or selected peers; changes apply only to this requester";
}

/** A rack line's subject, and the first of its details whose value changes. */
ChangeRow rackRow (const juce::String& line)
{
    const auto subject = line.upToFirstOccurrenceOf (";", false, false).upToFirstOccurrenceOf (" (", false, false).upToFirstOccurrenceOf (" after ", false, false);
    ChangeRow row { subject.toUpperCase(), {}, {} };
    for (const auto& part : juce::StringArray::fromTokens (line.fromFirstOccurrenceOf (";", false, false), ";", ""))
    {
        const auto values = part.trim().fromFirstOccurrenceOf (" ", false, false);
        row.before = values.upToFirstOccurrenceOf (" -> ", false, false);
        row.after = values.fromFirstOccurrenceOf (" -> ", false, false);
        if (row.before != row.after)
            break;
    }
    return row;
}

/** A changed note's row by the first of its fields that changes; ABSENT or REMOVED when it comes or goes. */
std::optional<ChangeRow> noteRow (int n, const juce::String& before, const juce::String& after)
{
    if (before == after)
        return {};
    const auto was = juce::StringArray::fromTokens (before, ",", ""), now = juce::StringArray::fromTokens (after, ",", "");
    ChangeRow row { "NOTE " + juce::String (n), before == "absent" ? juce::String ("ABSENT") : was[0].trim(), after == "removed" ? juce::String ("REMOVED") : now[0].trim() };
    if (before != "absent" && after != "removed")
        for (int i = 0; i < was.size() && i < now.size(); ++i)
            if (was[i].trim() != now[i].trim())
            {
                row.before = was[i].trim();
                row.after = now[i].trim();
                break;
            }
    return row;
}

juce::String noteCount (int count) { return juce::String (count) + (count == 1 ? " NOTE" : " NOTES"); }

juce::String clientCount (const McpView& v) { return juce::String (v.clients) + (v.clients == 1 ? " client" : " clients"); }

const juce::String mcpSetup = "Setup: docs/user-guide/mcp.md in the AIR repository";

/** A plate legend's outline: the screen face at the legend's cap height, scaled across. */
juce::Path legendPath (const Lettering& l, const juce::String& text, float cap, float hscale)
{
    auto p = Lettering::path (l.fontForCap (cap), text);
    p.applyTransform (juce::AffineTransform::scale (hscale, 1.0f));
    return p;
}

/** What the plate would print for the MCP light, as it prints LINKED INSTANCES': LINKED's unlit LED moved `across`
    whole pixels, so it stays as sharp, and fading at its edge into the plate around it; and the legend. It lies under
    the light, which draws only the lit LED and its states. */
class McpPrint final : public juce::Component
{
public:
    McpPrint (const juce::Image& plate, Pt led, int across, const juce::Path& legend)
    {
        setInterceptsMouseClicks (false, false);
        constexpr float solid = 9.5f, edge = 11.5f;
        const auto from = Rect (2.0f * edge, 2.0f * edge).withCentre (led).getSmallestIntegerContainer();
        const auto area = from.translated (across, 0).getUnion (legend.getBounds().getSmallestIntegerContainer().expanded (1));
        setBounds (area);
        print = juce::Image (juce::Image::ARGB, area.getWidth(), area.getHeight(), true);
        for (int y = from.getY(); y < from.getBottom(); ++y)
            for (int x = from.getX(); x < from.getRight(); ++x)
            {
                const auto d = led.getDistanceFrom ({ (float) x + 0.5f, (float) y + 0.5f });
                const auto alpha = juce::jlimit (0.0f, 1.0f, (edge - d) / (edge - solid));
                print.setPixelAt (x + across - area.getX(), y - area.getY(), plate.getPixelAt (x, y).withMultipliedAlpha (alpha));
            }
        juce::Graphics g (print);
        const auto at = juce::AffineTransform::translation (-area.getPosition().toFloat());
        // Light with a dark edge, as the drawing's sharpening left the plate's legends.
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.strokePath (legend, juce::PathStrokeType (1.2f), at);
        g.setColour (juce::Colour (0xfff2f2f0));
        g.fillPath (legend, at);
    }

    void paint (juce::Graphics& g) override { g.drawImageAt (print, 0, 0); }

private:
    juce::Image print;
};
} // namespace

AiUnit::AiUnit (InstrumentProcessor& p, const Model& model, StudioServices s)
    : processor (p), services (std::move (s)), view (model, "ai")
{
    identify (*this, "ai.panel", "Native AI proposal workflow");
    addAndMakeVisible (view);
    setSize (view.getWidth(), view.getHeight());
    {
        // MCP lettered as LINKED INSTANCES is, its LED as far from its legend, centred on the bottom row between the
        // left screw (whose shadow ends at x 36) and LINKED's LED.
        const auto& linked = model.element ("ai.linked");
        const auto cap = linked.legend.getHeight(), gap = linked.legend.getX() - linked.led.x;
        const auto hscale = linked.legend.getWidth() / legendPath (view.screenLettering, linked.label, cap, 1.0f).getBounds().getWidth();
        auto legend = legendPath (view.screenLettering, "MCP", cap, hscale);
        const auto left = linked.lensR, width = left + gap + legend.getBounds().getWidth();
        const auto across = juce::roundToInt ((36.0f + linked.led.x - linked.lensR - width) / 2.0f + left - linked.led.x);
        mcpElement = linked;
        mcpElement.id = mcpElement.group = "ai.mcp";
        mcpElement.label = "MCP";
        mcpElement.led = linked.led.translated ((float) across, 0.0f);
        legend.applyTransform (juce::AffineTransform::translation (mcpElement.led.x + gap - legend.getBounds().getX(),
                                                                   linked.legend.getBottom() - legend.getBounds().getBottom()));
        mcpElement.legend = Rect::leftTopRightBottom (legend.getBounds().getX(), linked.legend.getY(), legend.getBounds().getRight(), linked.legend.getBottom());
        mcpElement.hitRect = Rect::leftTopRightBottom (linked.hitRect.getX() + (float) across, linked.hitRect.getY(),
                                                       mcpElement.legend.getRight() + linked.hitRect.getRight() - linked.legend.getRight(), linked.hitRect.getBottom());
        mcpPrint = std::make_unique<McpPrint> (model.plates.at ("ai"), linked.led, across, legend);
        addAndMakeVisible (*mcpPrint);
        mcpLight = std::make_unique<Target> (view.ctx, mcpElement);
        mcpLight->setComponentID ("ai.mcp");
        mcpLight->onClick = [this] { toggleMcp(); };
        addAndMakeVisible (*mcpLight);
    }
    identify (status, "ai.status", "Account, workflow and captured audition status");
    identify (groupStatus, "group.status", "Linked group and peer context");
    identify (difference, "ai.diff", "All intended physical parameter, rack, macro and pattern changes");
    difference.setMultiLine (true);
    difference.setReadOnly (true);
    addChildComponent (status);
    addChildComponent (groupStatus);
    addChildComponent (difference);
    const juce::Component::SafePointer<AiUnit> owner (this);
    processor.groupControl->panelOwner = this;
    processor.groupControl->refreshPanel = [owner] { if (auto* unit = owner.getComponent()) unit->refresh(); };
    for (const auto& led : std::initializer_list<std::pair<const char*, const bool*>> { { "ai.generate", &leds.generate }, { "ai.refine", &leds.refine },
                                                                                    { "ai.preview", &leds.audition }, { "ai.apply", &leds.apply } })
        if (auto* b = dynamic_cast<SkinButton*> (button (led.first)))
            b->litWhen = [on = led.second] { return *on; };
    if (auto* b = button ("ai.generate"))
        b->onClick = [this] { generate(); };
    if (auto* b = button ("ai.refine"))
        b->onClick = [this] { refine(); };
    if (auto* b = button ("ai.preview"))
        b->onClick = [this] { audition(); };
    if (auto* b = button ("ai.stop"))
        b->onClick = [this] { stop(); };
    if (auto* b = button ("ai.apply"))
        b->onClick = [this] { apply(); };
    if (auto* b = button ("ai.discard"))
        b->onClick = [this] { discard(); };
    if (auto* b = button ("ai.account"))
        b->onClick = [this] { toggleAccount(); };
    if (auto* b = button ("ai.model"))
        b->onClick = [this] { toggleAccount(); };
    if (auto* b = button ("ai.linked"))
        b->onClick = [this] { toggleLinked(); };
    if (auto* screen = view.screen.get())
    {
        screen->onCommand = [this] (const juce::String& c, int value) { command (c, value); };
        screen->onEntry = [this] (const juce::String& c, const juce::String& t) { entered (c, t); };
        screen->onEscape = [this]
        {
            pressMessage.clear();
            page = "compose";
            refresh();
        };
    }
    startTimer (25);
    refresh();
}

AiUnit::~AiUnit(){retire();}

void AiUnit::retire()
{
    if(retired)return;retired=true;stopTimer();
    mcpLight->onClick=nullptr;
    for(auto& button:view.buttons)button->onClick=nullptr;
    // Removing the view commits an open entry, which must not reach a unit that is going.
    if (auto* screen = view.screen.get())
    {
        screen->onCommand = nullptr;
        screen->onEntry = nullptr;
        screen->onEscape = nullptr;
    }
    if (processor.groupControl->panelOwner == this)
    {
        processor.groupControl->refreshPanel = {};
        processor.groupControl->panelOwner = nullptr;
    }
    processor.closeAiPanel();
}

void AiUnit::visibilityChanged()
{
    if (retired || ! isVisible())
        return;
    const auto phase = processor.aiWorkflowStatus().connection.phase;
    if (phase == companion::ClientPhase::closed || phase == companion::ClientPhase::failed)
        processor.requestAiAccount ({ companion::AccountAction::status, {} });
    refresh();
}

juce::Button* AiUnit::button (const char* id) const { return dynamic_cast<juce::Button*> (view.findChildWithID (id)); }

AiUnit::Facts AiUnit::facts() const
{
    Facts f;
    f.workflow = processor.aiWorkflowStatus();
    f.account = processor.aiAccountReply();
    f.group = processor.groupWorkflowView();
    f.phase = processor.aiProposalPhase();
    f.result = processor.aiProposal();
    f.request = processor.aiCurrentRequest();
    const auto* a = f.account;
    f.signedIn = a != nullptr && a->status == companion::RuntimeStatus::ready && a->providerStatus == rack::oauth::Status::connected;
    f.connected = f.signedIn && f.workflow.connection.phase == companion::ClientPhase::ready && ! f.workflow.accountPending;
    f.selected = f.connected && a->selectedModel.isNotEmpty()
                 && std::any_of (a->models.begin(), a->models.end(), [a] (const auto& m) { return m.slug == a->selectedModel; });
    f.proposed = f.phase == AiProposalPhase::proposed;
    f.eligible = f.group.link.phase == GroupLinkPhase::authorized || f.group.link.phase == GroupLinkPhase::admitted;
    f.audioReady = f.result != nullptr && f.result->status == ai::GenerationStatus::ready && f.result->candidate.has_value();
    f.stopped = processor.aiPreviewStopped();
    f.playing = processor.aiPreviewPlaying();
    if (const auto* intent = processor.aiIntendedEdits())
        f.name = intent->name;
    else if (f.result != nullptr)
        f.name = f.result->edits.name;
    {
        const std::lock_guard lock (processor.restoreMutex);
        if (const auto& marker = processor.groupControl->marker)
        {
            f.actualGrouped = true;
            f.refinable = processor.groupMarkerCurrentOwned (*marker) && ! f.group.busy && ! f.group.conflicted;
        }
    }
    f.current = ! f.actualGrouped || (f.group.peerContextCurrent && ! f.group.busy && ! f.group.conflicted);
    return f;
}

AiUnit::Stamp AiUnit::stampNow() const
{
    const auto* r = processor.aiCurrentRequest();
    return { r != nullptr ? r->tag.requestId : 0, processor.aiProposal(), processor.aiProposalPhase() };
}

void AiUnit::refuse (const juce::String& reason)
{
    pressMessage = reason;
    pressStamp = stampNow();
}

void AiUnit::toggleAccount()
{
    const auto current=facts();
    if(page!="account"&&current.account&&!current.account->accounts.empty()){showAccounts();return;}
    pressMessage.clear();
    if (page == "account")
        page = "compose";
    else
    {
        page = "account";
        const auto f = facts();
        const auto* a = f.account;
        const bool signedOut = a != nullptr && (a->providerStatus == rack::oauth::Status::signedOut || a->providerStatus == rack::oauth::Status::accessExpired);
        if (! signedOut && ! f.workflow.accountPending && f.phase != AiProposalPhase::pending)
            processor.requestAiAccount ({ companion::AccountAction::status, {} });
    }
    refresh();
}

void AiUnit::toggleLinked()
{
    pressMessage.clear();
    page = page == "linked" || page == "join" ? "compose" : "linked";
    refresh();
}

void AiUnit::toggleMcp()
{
    pressMessage.clear();
    page = page == "mcp" ? "compose" : "mcp";
    refresh();
}

void AiUnit::command (const juce::String& c, int value)
{
    pressMessage.clear();
    if (c == "ai.scope")
    {
        scope = value;
        requirePattern = scope != 0;
    }
    else if (c == "ai.requirePattern") requirePattern = value != 0;
    else if (c == "ai.allowMacros") macros = value != 0;
    else if (c == "ai.allowReuse") reuse = value != 0;
    else if (c == "ai.model") chooseModel (value);
    else if (c == "ai.account") accountButton();
    else if (c == "ai.account.add") addAccount();
    else if (c == "ai.account.choose") {const auto f=facts();if(f.account&&value>=0&&std::size_t(value)<f.account->accounts.size())chooseAccount(f.account->accounts[std::size_t(value)].id);}
    else if (c == "ai.cancel") cancel();
    else if (c == "ai.page.join") page = "join";
    else if (c == "group.join") joinGroup (value);
    else if (c == "group.refresh") actionMessage = processor.browseGroups() ? juce::String() : "Companion unavailable; manual sound stays editable";
    else if (c == "group.resume")
        actionMessage = processor.resumeGroupIntent() && processor.requestGroupConnection() ? juce::String() : "Unable to resume; manual sound stays editable";
    else if (c == "group.leave")
    {
        processor.leaveGroupIntent();
        actionMessage = {};
    }
    else if (c == "group.peer") selectPeer (value);
    else if (c == "mcp.access") processor.setMcpEnabled (! processor.mcpView().enabled);
    refresh();
}

void AiUnit::entered (const juce::String& c, const juce::String& t)
{
    pressMessage.clear();
    if (c == "ai.prompt") prompt = t;
    else if (c == "ai.role" || c == "ai.key" || c == "ai.scale" || c == "ai.tempo")
    {
        (c == "ai.role" ? role : c == "ai.key" ? key : c == "ai.scale" ? scale : tempo) = t;
        updateContext();
    }
    else if (c == "group.name")
    {
        groupName = t;
        link (GroupUuid (juce::Uuid()), t.trim());
    }
    refresh();
}

void AiUnit::generate()
{
    pressMessage.clear();
    const auto f = facts();
    if (! contextValid) refuse (contextError);
    else if (! f.connected) refuse ("Sign in and connect to generate");
    else if (! f.selected) refuse ("Choose a model to generate");
    else if (prompt.trim().isEmpty()) refuse ("Describe a sound or a change to generate");
    else if (f.eligible && ! f.group.generationReady && ! f.group.generationWaits) refuse (groupWait (f.group));
    else if (f.phase == AiProposalPhase::pending) refuse ("Already generating; Cancel stops it");
    else
    {
        processor.generateAi ({ prompt.toStdString(), f.account->selectedModel, { static_cast<ai::RequestScope> (scope), macros, reuse, requirePattern } });
        page = "compose";
    }
    refresh();
}

void AiUnit::refine()
{
    pressMessage.clear();
    const auto f = facts();
    if (! contextValid) refuse (contextError);
    else if (! f.connected) refuse ("Sign in and connect to refine");
    else if (! f.selected) refuse ("Choose a model to refine");
    else if (prompt.trim().isEmpty()) refuse ("Describe a change to refine");
    else if (! f.proposed || f.result == nullptr) refuse ("Refine needs a proposal: Generate first");
    else if (! f.refinable) refuse ("Refine waits for the group: finish edits and resolve conflicts");
    else if (f.workflow.applyPending) refuse ("Apply is waiting; Discard cancels it");
    else
    {
        processor.refineAi (prompt.toStdString());
        page = "compose";
    }
    refresh();
}

void AiUnit::audition()
{
    pressMessage.clear();
    const auto f = facts();
    if (! contextValid) refuse (contextError);
    else if (! f.proposed || f.result == nullptr) refuse ("Nothing to audition yet");
    else if (! f.current) refuse ("Audition waits for fresh peer context");
    else if (! f.stopped) refuse ("Already auditioning; Stop returns to the live sound");
    else if (f.workflow.applyPending) refuse ("Apply is waiting; Discard cancels it");
    else processor.startAiPreview();
    refresh();
}

void AiUnit::stop()
{
    pressMessage.clear();
    if (processor.aiPreviewStopped()) refuse ("Nothing is auditioning");
    else processor.stopAiPreview();
    refresh();
}

void AiUnit::apply()
{
    pressMessage.clear();
    const auto f = facts();
    if (! contextValid) refuse (contextError);
    else if (! f.proposed || f.result == nullptr) refuse ("Nothing to apply");
    else if (! f.current) refuse ("Apply waits for fresh peer context");
    else if (f.workflow.applyPending) refuse ("Apply is waiting; Discard cancels it");
    else processor.applyAiWhenStopped();
    refresh();
}

void AiUnit::discard()
{
    pressMessage.clear();
    const auto f = facts();
    if (! (f.proposed || f.phase == AiProposalPhase::pending || f.workflow.applyPending || ! f.stopped))
        refuse ("Nothing to discard");
    else processor.discardAiProposal();
    refresh();
}

void AiUnit::cancel()
{
    processor.cancelAi();
    signingIn = false;
    refresh();
}

void AiUnit::accountButton()
{
    if (view.screen != nullptr && view.screen->shown.button == "CANCEL")
        return cancel();
    const auto f = facts();
    if (f.phase == AiProposalPhase::pending)
        return refuse ("Account waits for the current request; Cancel stops it");
    const auto* a = f.account;
    if(a != nullptr && (a->providerStatus == rack::oauth::Status::inferenceNotGranted || a->providerStatus == rack::oauth::Status::denied || a->providerStatus == rack::oauth::Status::expiredAttempt))
    {showAccounts();return;}
    const bool signIn = a != nullptr && ! f.signedIn;
    if(signIn&&!a->accounts.empty()){showAccounts();return;}
    processor.requestAiAccount ({ signIn ? companion::AccountAction::signIn : companion::AccountAction::status, {} });
    signingIn = signIn;
}
void AiUnit::showAccounts()
{
    const auto f=facts();if(f.phase==AiProposalPhase::pending||f.workflow.accountPending)return refuse("Account waits for the current request; Cancel stops it");
    const auto accounts=f.account?f.account->accounts:std::vector<rack::oauth::SavedAccount>{};
    juce::PopupMenu menu;for(std::size_t i=0;i<accounts.size();++i)menu.addItem(int(i)+1,accounts[i].label,true,f.account&&accounts[i].id==f.account->activeAccount);
    if(!accounts.empty())menu.addSeparator();
    menu.addItem(1000,"Use another account...");menu.addItem(1001,"Generation settings");
    const juce::Component::SafePointer<AiUnit> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),[safe,accounts](int choice){
        if(!safe||safe->retired)return;
        if(choice>=1&&std::size_t(choice)<=accounts.size())safe->chooseAccount(accounts[std::size_t(choice-1)].id);
        else if(choice==1000)safe->addAccount();
        else if(choice==1001){safe->page="account";safe->refresh();}
    });
}
void AiUnit::chooseAccount(const juce::String& id)
{
    const auto f=facts();if(retired||f.phase==AiProposalPhase::pending||f.workflow.accountPending)return refuse("Account waits for the current request; Cancel stops it");
    if(processor.requestAiAccount({companion::AccountAction::selectAccount,id})){signingIn=true;page="account";pressMessage.clear();}
}
void AiUnit::addAccount()
{
    const auto f=facts();if(retired||f.phase==AiProposalPhase::pending||f.workflow.accountPending)return refuse("Account waits for the current request; Cancel stops it");
    if(processor.requestAiAccount({companion::AccountAction::addAccount,{}})){signingIn=true;page="account";pressMessage.clear();}
}

void AiUnit::chooseModel (int row)
{
    const auto f = facts();
    if (f.phase == AiProposalPhase::pending || f.workflow.accountPending)
        refuse ("The model stays while a request is pending; Cancel stops it");
    else if (row >= 0 && std::size_t (row) < modelSlugs.size())
        processor.requestAiAccount ({ companion::AccountAction::selectModel, modelSlugs[std::size_t (row)] });
}

void AiUnit::joinGroup (int row)
{
    const auto group = processor.groupWorkflowView();
    const auto chosen = row >= 0 && std::size_t (row) < choices.size() ? choices[std::size_t (row)].id : GroupUuid {};
    const auto member = std::find_if (group.members.begin(), group.members.end(),
                                      [&] (const auto& m) { return ! chosen.isNull() && m.intent.group == chosen; });
    if (member == group.members.end())
        actionMessage = "Choose a currently discovered group before Join";
    else
        link (member->intent.group, text (member->intent.name));
    if (actionMessage.isEmpty())
        page = "linked";
}

void AiUnit::link (GroupUuid group, const juce::String& name)
{
    const auto label = name.toStdString();
    GroupIntent intent;
    intent.group = group;
    intent.member = juce::Uuid();
    const auto context = processor.aiMusicalContext();
    if (! setGroupText (label, intent.name) || label.empty())
        actionMessage = "Group name must contain 1 to 128 UTF-8 bytes";
    else if (! setGroupText (context.role, intent.role) || ! setGroupText (context.key, intent.key) || ! setGroupText (context.scale, intent.scale))
        actionMessage = "Repair the requester musical context before linking";
    else
    {
        intent.bpm = context.auditionBpm;
        actionMessage = processor.setGroupIntent (intent) && processor.requestGroupConnection() ? juce::String() : "Unable to link; manual sound stays editable";
    }
}

void AiUnit::selectPeer (int row)
{
    if (row < 0 || std::size_t (row) >= peerMembers.size())
        return;
    const auto group = processor.groupWorkflowView();
    const auto member = peerMembers[std::size_t (row)];
    std::vector<GroupUuid> selected (group.selection.members.begin(), group.selection.members.begin() + group.selection.peerCount);
    const auto found = std::find (selected.begin(), selected.end(), member);
    if (found != selected.end())
        selected.erase (found);
    else
        selected.push_back (member);
    if (selected.size() > maximumSelectedGroupPeers)
        actionMessage = "Select at most 4 peers; remove one before adding another";
    else
        actionMessage = processor.selectGroupPeers (selected) ? juce::String() : "Peer unavailable; refresh or remove the missing selection";
}

void AiUnit::updateContext()
{
    AiMusicalContext context;
    context.role = role.toStdString();
    context.key = key.toStdString();
    context.scale = scale.toStdString();
    const auto value = tempo.trim().toStdString();
    if (! value.empty())
    {
        double bpm = 0;
        const auto parsed = std::from_chars (value.data(), value.data() + value.size(), bpm);
        if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() || ! std::isfinite (bpm) || bpm < 20 || bpm > 400)
        {
            contextValid = false;
            contextError = "Invalid tempo: enter 20 to 400 BPM, or leave blank";
            return;
        }
        context.auditionBpm = bpm;
    }
    contextValid = processor.setAiMusicalContext (context);
    contextError = contextValid ? juce::String() : "Invalid context: role, key and scale must be valid text within 128 UTF-8 bytes";
}

juce::String AiUnit::todaysSentence (const Facts& f) const
{
    const auto& workflow = f.workflow;
    const auto& group = f.group;
    auto message = contextValid ? workflow.message : contextError;
    if (message.isEmpty())
        message = "AI offline; expand and click Account to connect";
    if (contextValid && f.account == nullptr && workflow.connection.phase == companion::ClientPhase::ready && ! workflow.accountPending
        && f.phase == AiProposalPhase::idle && message == "Connected; model compatibility verified")
        message = "Click Account, then choose a model";
    if (contextValid && f.eligible && ! group.generationReady && ! group.generationWaits)
        message = groupWait (group);
    if (contextValid && f.actualGrouped)
        message += " / requester-only proposal; Apply checks fresh peer context";
    if (contextValid && group.link.phase == GroupLinkPhase::pending)
        message += " / saved group pending; Resume explicitly to link";
    if (const auto* result = f.result; result != nullptr && result->status == ai::GenerationStatus::ready)
    {
        const auto& context = result->audio.context;
        message += "\n";
        message += context.phraseSource == ai::PhraseSource::proposed ? "Proposed phrase" : context.phraseSource == ai::PhraseSource::captured ? "Captured phrase" : "Default notes";
        message += " / " + juce::String (context.bpm, 1) + " BPM " + (context.tempoSource == ai::TempoSource::captured ? "captured" : "default")
                   + (result->audio.phraseTruncated ? " / truncated" : "");
    }
    if (! f.stopped && ! workflow.applyPending)
        message += f.playing ? " / Preview playing" : " / Stop awaiting audio";
    return message;
}

juce::String AiUnit::proposalDifference (std::vector<ChangeRow>& rows) const
{
    const auto* result = processor.aiProposal();
    const auto* source = processor.aiOriginalSource();
    const auto* intent = processor.aiIntendedEdits();
    const auto* request = processor.aiCurrentRequest();
    if (! intent && result)
        intent = &result->edits;
    if (! result || ! source || ! intent || ! request)
        return "Proposal changes appear here. Committed sound stays editable; existing DAW automation remains attached.";
    constexpr const char* scopes[] { "Sound", "Pattern", "Both" };
    juce::String out = "Captured policy: " + juce::String (scopes[static_cast<int> (request->policy.scope)]) + "; macros "
                       + (request->policy.allowMacroChanges ? "allowed" : "preserved") + "; bank reuse " + (request->policy.allowSlotReuse ? "allowed" : "disabled")
                       + "; notes " + (request->policy.requirePattern ? "required" : "optional") + "\n";
    out += "Existing DAW automation remains attached to stable physical controls. Explicit equal-base parameter intent is retained.\n\n";
    if (! result->candidate)
        return out;
    SoundDocument reviewed;
    RackDifference rackDiff;
    auto displayChoices = intent->reconciliation;
    reconcileRackEdit (source->sound, result->candidate->sound, displayChoices, SoundEditOrigin::manual, reviewed, rackDiff);
    out += "Sonic: " + rackDiff.sonic + "\nPresentation: " + rackDiff.presentation + "\nStructural: " + rackDiff.structural + "\n" + rackDiff.automationWarning + "\n";
    const auto registry = parameterRegistry();
    const auto& knobNames = view.model.knobNames;
    auto physical = [&] (std::size_t n, float v)
    {
        auto t = processor.control (n).getText (v, 64);
        if (registry[n].unit.isNotEmpty() && ! t.contains (registry[n].unit))
            t += " " + registry[n].unit;
        return t;
    };
    if (request->catalog && result->candidate->sound.personalities)
        for (const auto& selection : intent->catalogSelections)
        {
            const auto id = ai::catalogInstanceId (request->catalog->root, selection.localId);
            const auto& instances = result->candidate->sound.personalities->instances;
            const auto found = std::find_if (instances.begin(), instances.end(), [&] (const auto& rack) { return rack.id == id; });
            if (found == instances.end())
                continue;
            const auto& rack = *found;
            const auto place = rackPlaceText (result->candidate->sound, rack);
            rows.push_back ({ rack.design.name, "Add", place });
            out += "Add " + rack.design.name + ": " + place + "\n";
            for (const auto& edit : intent->catalogParameters)
            {
                if (edit.selection != selection.localId)
                    continue;
                const auto unit = std::find_if (rack.design.units.begin(), rack.design.units.end(), [&] (const auto& u) { return u.id == edit.unit; });
                const auto* binding = rackUnitBinding (rack, edit.unit);
                if (unit == rack.design.units.end() || ! binding)
                    continue;
                const auto n = rackParameterIndex (unit->module, binding->slot, edit.key);
                if (! n)
                    continue;
                const auto control = std::find_if (rack.design.panel.controls.begin(), rack.design.panel.controls.end(), [&] (const auto& c) { return c.unit == edit.unit && c.key == edit.key; });
                const auto label = control != rack.design.panel.controls.end() ? control->label
                    : knobNames[*n].isNotEmpty() ? knobNames[*n] : registry[*n].name;
                const auto after = physical (*n, result->candidate->sound.values[*n]);
                rows.push_back ({ label, "-", after });
                out += rack.design.name + " / " + label + ": " + after + "\n";
            }
        }
    for (const auto& edit : intent->parameters)
    {
        const auto it = std::find_if (registry.begin(), registry.end(), [&] (const auto& d) { return d.id == edit.id; });
        if (it == registry.end())
            continue;
        const auto n = std::size_t (it - registry.begin());
        const auto before = physical (n, source->sound.values[n]), after = physical (n, result->candidate->sound.values[n]);
        out += it->name + " [" + edit.id + "]: " + before + " -> " + after;
        if (source->sound.values[n] == result->candidate->sound.values[n])
            out += " (explicit intent)";
        out += "\n";
        rows.push_back ({ knobNames[n].isNotEmpty() ? knobNames[n] : it->name.toUpperCase(), before, after, source->sound.controlLocks[n] });
    }
    for (const auto& edit : intent->macros)
    {
        const auto old = source->sound.macros[edit.index - 1];
        const auto oldIndex = parameterIndexForKey (old.targetKey);
        const auto target = std::find_if (registry.begin(), registry.end(), [&] (const auto& d) { return d.id == edit.targetId; });
        const auto oldName = oldIndex ? registry[*oldIndex].name : juce::String ("Unassigned");
        const auto newName = target == registry.end() ? juce::String ("Unassigned") : target->name;
        out += "Macro " + juce::String (edit.index) + " target " + oldName + " -> " + newName + "; depth " + juce::String (old.amount, 3) + " -> " + juce::String (edit.amount, 3) + "\n";
        const auto subject = "MACRO " + juce::String (edit.index);
        rows.push_back (oldName != newName ? ChangeRow { subject, oldName, newName } : ChangeRow { subject, juce::String (old.amount, 3), juce::String (edit.amount, 3) });
    }
    auto layout = source->sound.layout;
    constexpr const char* rackNames[] { "Add synth", "Remove synth", "Move synth", "Add effect", "Remove effect", "Move effect", "Replace effect" };
    constexpr const char* states[] { "never used", "present", "retired" };
    auto position = [] (const auto& order, std::uint8_t count, std::uint8_t slot)
    {
        const auto found = std::find (order.begin(), order.begin() + count, slot);
        return found == order.begin() + count ? juce::String ("none") : juce::String (int (found - order.begin() + 1));
    };
    auto effectDifference = [&] (const RackLayout& before, const RackLayout& after, std::uint8_t slot)
    {
        const auto& old = before.effects[slot];
        const auto& next = after.effects[slot];
        return "kind " + (old.use == SlotUse::neverUsed ? juce::String ("absent") : effectDisplayName (old.kind)) + " -> "
               + (next.use == SlotUse::neverUsed ? juce::String ("absent") : effectDisplayName (next.kind)) + "; state " + states[static_cast<int> (old.use)]
               + " -> " + states[static_cast<int> (next.use)] + "; owner " + (old.use == SlotUse::present ? ownerName (old.chain) : juce::String ("none")) + " -> "
               + (next.use == SlotUse::present ? ownerName (next.chain) : juce::String ("none")) + "; position "
               + (old.use == SlotUse::present ? position (before.effectOrder[old.chain], before.effectCount[old.chain], slot) : juce::String ("none")) + " -> "
               + (next.use == SlotUse::present ? position (after.effectOrder[next.chain], after.effectCount[next.chain], slot) : juce::String ("none"));
    };
    for (const auto& raw : intent->racks)
    {
        auto edit = raw;
        edit.allowReuse = request->policy.allowSlotReuse;
        const auto before = layout;
        editRackLayout (before, edit, layout);
        const bool synth = edit.kind <= RackEditKind::moveSynth;
        auto slot = edit.slot;
        if (slot == noSlot)
            for (std::uint8_t n = 0; n < (synth ? before.synthUse.size() : before.effects.size()); ++n)
                if ((synth ? before.synthUse[n] : before.effects[n].use) != SlotUse::present && (synth ? layout.synthUse[n] : layout.effects[n].use) == SlotUse::present)
                {
                    slot = n;
                    break;
                }
        const bool add = edit.kind == RackEditKind::addSynth || edit.kind == RackEditKind::addEffect;
        const auto previousUse = synth ? before.synthUse[slot] : before.effects[slot].use;
        auto line = juce::String (rackNames[static_cast<int> (edit.kind)]) + " bank " + juce::String (slot + 1)
                    + (edit.slot == noSlot ? " (automatic selection)" : add && previousUse == SlotUse::retired ? " (explicit reuse)" : "") + "; ";
        if (synth)
            line += "state " + juce::String (states[static_cast<int> (previousUse)]) + " -> " + states[static_cast<int> (layout.synthUse[slot])] + "; position "
                    + position (before.synthOrder, before.synthCount, slot) + " -> " + position (layout.synthOrder, layout.synthCount, slot);
        else
            line += effectDifference (before, layout, slot);
        out += line + "\n";
        rows.push_back (rackRow (line));
        if (edit.kind == RackEditKind::removeSynth)
            for (std::uint8_t n = 0; n < before.effectCount[slot]; ++n)
            {
                const auto effect = before.effectOrder[slot][n];
                const auto retired = "Retire effect bank " + juce::String (effect + 1) + " after Remove synth bank " + juce::String (slot + 1) + "; "
                                     + effectDifference (before, layout, effect);
                out += retired + "\n";
                rows.push_back (rackRow (retired));
            }
    }
    if (! intent->racks.empty())
        out += "Physical bank reuse warning: any existing DAW automation keeps its named bank/control attachment.\n";
    if (intent->pattern)
    {
        const auto& was = source->pattern;
        const auto& now = result->candidate->pattern;
        out += "\nPattern old -> new: " + juce::String (was.noteCount) + " -> " + juce::String (now.noteCount) + " notes; meter " + juce::String (was.meter.numerator) + "/"
               + juce::String (was.meter.denominator) + " -> " + juce::String (now.meter.numerator) + "/" + juce::String (now.meter.denominator) + "; bars "
               + juce::String (was.bars) + " -> " + juce::String (now.bars) + "\n";
        rows.push_back ({ "PATTERN", noteCount (int (was.noteCount)), noteCount (int (now.noteCount)) });
        const auto count = std::max (was.noteCount, now.noteCount);
        for (std::size_t n = 0; n < count; ++n)
        {
            const auto before = n < was.noteCount ? noteText (was.notes[n]) : juce::String ("absent");
            const auto after = n < now.noteCount ? noteText (now.notes[n]) : juce::String ("removed");
            out += "Note " + juce::String (int (n + 1)) + ": " + before + " -> " + after + "\n";
            if (auto row = noteRow (int (n + 1), before, after))
                rows.push_back (*row);
        }
    }
    const auto& audio = result->audio;
    const auto& m = result->measurements;
    out += "\nAudition: "
           + juce::String (audio.context.phraseSource == ai::PhraseSource::proposed ? "proposed phrase" : audio.context.phraseSource == ai::PhraseSource::captured ? "captured phrase" : "default notes")
           + ", " + juce::String (audio.context.bpm, 2) + " BPM (" + (audio.context.tempoSource == ai::TempoSource::captured ? "captured" : "default 120") + ")"
           + (audio.phraseTruncated ? "; phrase truncated to 8 seconds" : "; complete phrase body") + "\n";
    out += "Peak " + juce::String (m.peak, 6) + "; RMS " + juce::String (m.rms, 6) + "; onset " + (m.onsetSeconds ? juce::String (*m.onsetSeconds, 3) : juce::String ("none"))
           + " s; decay " + juce::String (m.decaySeconds, 3) + " s; activity end " + juce::String (m.activityEndSeconds, 3) + " s\n";
    out += "Band fractions " + juce::String (m.bandFractions[0], 3) + " / " + juce::String (m.bandFractions[1], 3) + " / " + juce::String (m.bandFractions[2], 3) + "; finite "
           + (m.finite ? "yes" : "no") + "; clips " + juce::String (int (m.clippedSamples)) + "; tail active " + (m.tailActiveAtEnd ? "yes" : "no") + "\n";
    if (intent->name.isNotEmpty())
        out += "Name: " + intent->name + "\n";
    out += "Characteristics: " + intent->characteristics + "\nExplanation: " + intent->explanation + "\nStyle interpretations carry no guarantee.";
    return out;
}

juce::String AiUnit::groupSentence (const GroupWorkflowView& v, std::vector<PeerRow>& rows)
{
    const bool pending = v.link.phase == GroupLinkPhase::pending;
    juce::String message;
    if (v.link.exhausted) message = "Link revision exhausted; manual sound stays editable";
    else if (pending) message = "Saved group is pending. Resume explicitly to link; standalone sound stays editable";
    else if (v.link.phase == GroupLinkPhase::admitted && v.connection.phase == companion::ClientPhase::ready)
        message = "Linked: " + text (v.link.intent->name) + ". Changes apply only to this requester";
    else if (v.link.intent) message = "Waiting for companion acknowledgment; manual sound stays editable";
    else message = "Create or discover and Join a group explicitly. Select up to 4 peers";
    if (v.connection.phase == companion::ClientPhase::failed)
        message = "Companion unavailable; retry linking or Leave. Manual sound stays editable";
    if (v.busy)
        message += " / busy editing";
    if (v.conflicted)
        message += " / conflicting musical settings; resolve before Generate";
    peerMembers.clear();
    const auto end = v.selection.members.begin() + v.selection.peerCount;
    const auto row = [&] (GroupUuid member, PeerRow r)
    {
        if (rows.size() >= 20)
            return;
        r.selected = std::find (v.selection.members.begin(), end, member) != end;
        rows.push_back (std::move (r));
        peerMembers.push_back (member);
    };
    if (v.link.intent)
    {
        for (const auto& m : v.members)
        {
            if (m.intent.group != v.link.intent->group || m.runtime == v.link.runtime || m.intent.member == v.link.intent->member)
                continue;
            const juce::String source = m.mode == PlaybackMode::externalMidi ? "External MIDI" : "Internal pattern";
            auto notes = "notes " + availability (m.noteAvailability) + " (" + juce::String (m.noteCount) + ")";
            if (m.noteAvailability != ObservedMidiAvailability::unavailable)
                notes += " / age " + juce::String (juce::uint64 (m.noteAgeMs)) + " ms";
            if (m.noteReason != ObservedMidiReason::none)
                notes += " / " + noteReason (m.noteReason);
            if (m.notesTruncated)
                notes += " / truncated";
            auto controls = "controls " + availability (m.controlAvailability);
            if (m.controlAvailability != ObservedMidiAvailability::unavailable)
                controls += " / age " + juce::String (juce::uint64 (m.controlAgeMs)) + " ms";
            if (m.controlReason != LiveControlReason::none)
                controls += " / " + controlReason (m.controlReason);
            const auto flags = juce::String (m.busy ? " / busy" : "") + (m.conflicted ? " / conflict" : "");
            const auto state = m.conflicted ? juce::String ("CONFLICTING SETTINGS")
                             : m.busy       ? juce::String ("BUSY EDITING")
                                            : "NOTES " + availability (m.noteAvailability).toUpperCase() + "  CONTROLS " + availability (m.controlAvailability).toUpperCase();
            row (m.intent.member, { text (m.intent.role).toUpperCase(), source.toUpperCase(), state,
                                    uuid (m.intent.member) + "\n" + source + " / " + notes + "\n" + controls + flags + "\nAge describes published/captured context" });
        }
        for (std::size_t i = 0; i < v.selection.peerCount; ++i)
        {
            const auto member = v.selection.members[i];
            const bool found = std::any_of (v.members.begin(), v.members.end(), [&] (const auto& m)
                                            { return m.intent.group == v.link.intent->group && m.intent.member == member && m.runtime != v.link.runtime; });
            if (! found)
            {
                row (member, { "MISSING PEER", {}, "UNAVAILABLE; CLICK TO REMOVE",
                               uuid (member) + "\n" + "Missing peer / unavailable context. Click to remove the selection" });
                message += " / selected peer missing";
            }
        }
    }
    if (actionMessage.isNotEmpty())
        message += "\n" + actionMessage;
    return message;
}

juce::String AiUnit::pageFor (const Facts& f) const
{
    if (page == "mcp")
        return page;
    if (f.workflow.connection.phase == companion::ClientPhase::failed)
        return "alert";
    if (page == "linked" || page == "join")
        return page;
    if (page == "account")
        return f.signedIn ? "connected" : "signed_out";
    if (f.phase == AiProposalPhase::pending || f.proposed)
        return "proposal";
    if (! f.signedIn)
        return "signed_out";
    return "create";
}

ScreenShown AiUnit::shownFor (const Facts& f, const McpView& mcp, const juce::String& sentence, const juce::String& groupText,
                              const juce::String& details, std::vector<ChangeRow> changes, std::vector<PeerRow> peers) const
{
    const auto& pages = view.model.screens["pages"];
    const auto drawnBody = [] (const juce::var& page)
    {
        juce::StringArray body;
        if (const auto* slots = page["body"].getArray())
            for (const auto& slot : *slots)
                body.add (slot["text"].toString());
        return body;
    };
    ScreenShown s;
    s.page = shownPage;
    s.prompt = prompt;
    s.context = { { "KEY", key }, { "SCALE", scale }, { "ROLE", role }, { "BPM", tempo } };
    s.options = { { "ai.scope", scope }, { "ai.requirePattern", int (requirePattern) }, { "ai.allowMacros", int (macros) }, { "ai.allowReuse", int (reuse) } };
    if (scope == 0)
        s.dim.add ("ai.requirePattern");
    s.status = pressMessage.isNotEmpty() ? pressMessage : sentence.upToFirstOccurrenceOf ("\n", false, false);
    const auto& w = f.workflow;
    if (s.page == "proposal")
    {
        s.title = f.phase == AiProposalPhase::pending ? "GENERATING"
                : f.proposed && f.playing             ? "PROPOSAL: AUDITIONING"
                : f.proposed                          ? "PROPOSAL READY"
                : f.phase == AiProposalPhase::stale   ? "PROPOSAL OUT OF DATE"
                                                      : "NO PROPOSAL";
        s.action = f.phase == AiProposalPhase::pending ? "CANCEL" : "";
        if (f.proposed)
        {
            s.changes = std::move (changes);
            s.name = f.name;
        }
    }
    else if (s.page == "alert")
    {
        const auto& P = pages["alert"];
        s.heading = P["heading"]["text"].toString();
        s.body = drawnBody (P);
        s.button = P["button"]["text"].toString();
    }
    else if (s.page == "signed_out")
    {
        const auto& P = pages["signed_out"];
        const auto body = drawnBody (P);
        const auto* a = f.account;
        const auto manual = body[body.size() - 1];
        const auto waiting = [&] (const char* heading, const char* line)
        {
            s.heading = heading;
            s.body.add (line);
            s.body.add ({});
            s.body.add (manual);
        };
        if (w.connection.phase == companion::ClientPhase::connecting || (a == nullptr && w.accountPending))
            waiting ("CONNECTING", "Waiting for the AI companion.");
        else if (w.accountPending && signingIn)
        {
            waiting ("SIGNING IN", "Finish signing in in your browser.");
            s.button = "CANCEL";
        }
        else if (w.accountPending && a != nullptr && a->providerStatus == rack::oauth::Status::accessExpired)
            waiting ("RENEWING", "Renewing your sign-in.");
        else
        {
            s.body = body;
            s.button = w.accountPending ? juce::String ("CANCEL") : P["button"]["text"].toString();
            if (a != nullptr && a->providerStatus == rack::oauth::Status::accessExpired)
            {
                s.heading = "SIGN-IN EXPIRED";
                s.body.set (0, "Sign in again to generate.");
            }
            else if(a != nullptr && (a->providerStatus == rack::oauth::Status::inferenceNotGranted || a->providerStatus == rack::oauth::Status::denied || a->providerStatus == rack::oauth::Status::expiredAttempt))
            {
                s.heading = a->providerStatus == rack::oauth::Status::inferenceNotGranted ? "GENERATION UNAVAILABLE"
                    : a->providerStatus == rack::oauth::Status::denied ? "PERMISSION NOT GRANTED" : "SIGN-IN INCOMPLETE";
                s.body.set (0, w.message);
                s.button = "CHOOSE ACCOUNT";
            }
            else
            {
                s.heading = "SIGNED OUT";
                if (a != nullptr && a->providerStatus != rack::oauth::Status::signedOut)
                    s.body.set (0, w.message);
            }
        }
    }
    else if (s.page == "connected")
    {
        s.heading = "CONNECTED";
        s.body.add (pressMessage.isNotEmpty() ? pressMessage : drawnBody (pages["connected"])[0]);
        if(pressMessage.isEmpty()&&f.account)for(const auto& account:f.account->accounts)if(account.id==f.account->activeAccount)s.body.set(0,account.label);
        for (const auto& name : modelNames)
            s.list.add (name);
        s.chosen = chosenModel;
    }
    else if (s.page == "join")
    {
        s.heading = "JOIN A GROUP";
        s.body.add (actionMessage.isNotEmpty() ? actionMessage
                    : pressMessage.isNotEmpty() ? pressMessage
                    : choices.empty()           ? juce::String ("No groups found yet; Refresh looks again.")
                                                : juce::String ("Choose a discovered group."));
        for (const auto& c : choices)
            s.list.add (c.name + " (" + uuid (c.id).substring (0, 8) + ")");
    }
    else if (s.page == "linked")
    {
        const auto& link = f.group.link;
        const bool pending = link.phase == GroupLinkPhase::pending;
        const auto name = link.intent ? text (link.intent->name).toUpperCase() : juce::String();
        s.heading = ! link.intent                            ? juce::String ("NOT LINKED")
                  : link.phase == GroupLinkPhase::admitted ? "LINKED: " + name
                  : pending                                ? "LINK PENDING: " + name
                                                           : "LINKING: " + name;
        const auto fallback = ! link.intent ? juce::String ("Create or join a group to link.")
                            : pending       ? juce::String ("Resume links this synth again.")
                                            : drawnBody (pages["linked"])[0];
        s.body.add (actionMessage.isNotEmpty() ? actionMessage : pressMessage.isNotEmpty() ? pressMessage : fallback);
        if (pending)
            s.joinLabel = "RESUME";
        s.leaveEnabled = link.intent.has_value();
        s.groupName = groupName;
        s.peers = std::move (peers);
    }
    else if (s.page == "mcp")
    {
        const auto running = ! mcp.instance.isNull();
        s.heading = ! running ? "MCP UNAVAILABLE" : mcp.enabled ? "MCP ACCESS ON" : "MCP ACCESS OFF";
        if (! running)
            s.body.add ("The MCP service is not running.");
        else
        {
            s.body.add (! mcp.enabled     ? "Agents cannot reach this synth."
                        : mcp.connected ? "Agents can read and edit this synth."
                                        : "Waiting for the MCP service.");
            s.body.add ("ID " + mcp.instance.toString().substring (0, 8)
                        + (mcp.clientsAvailable ? ", " + clientCount (mcp) + ", " + juce::String (mcp.requests) + (mcp.requests == 1 ? " request" : " requests")
                                                : juce::String()));
            // lastEdit opens "Transaction N", then its revision and changes.
            const auto edit = juce::StringArray::fromTokens (mcp.lastEdit.upToFirstOccurrenceOf ("\n", false, false), " ", "");
            s.body.add (mcp.lastEdit.isEmpty() ? juce::String ("No agent edit yet.") : "Last agent edit: transaction " + edit[1] + ".");
            s.button = mcp.enabled ? "DISABLE ACCESS" : "ENABLE ACCESS";
        }
        s.status = mcpSetup;
    }
    s.spoken = sentence;
    if (s.page == "mcp")
        s.spoken = (mcp.instance.isNull() ? juce::String() : "Instance " + mcp.instance.toDashedString() + ", " + mcp.label + ". ")
                   + (mcp.lastEdit.isEmpty() ? juce::String() : mcp.lastEdit + "\n") + mcpSetup;
    if (s.page == "proposal")
        s.spoken += "\n" + details;
    if (s.page == "linked" || s.page == "join")
        s.spoken += "\n" + groupText;
    return s;
}

void AiUnit::refresh()
{
    if(retired)return;
    if (contextValid)
    {
        const auto context = processor.aiMusicalContext();
        role = context.role;
        key = context.key;
        scale = context.scale;
        tempo = context.auditionBpm ? juce::String (*context.auditionBpm) : juce::String();
    }
    const auto f = facts();
    const auto* a = f.account;
    if (! f.workflow.accountPending)
        signingIn = false;
    modelSlugs.clear();
    modelNames.clear();
    if (a != nullptr)
        for (const auto& m : a->models)
        {
            modelSlugs.push_back (m.slug);
            modelNames.push_back (m.displayName.isEmpty() ? m.slug : m.displayName + " (" + m.slug + ")");
        }
    if (! f.workflow.accountPending)
    {
        const auto at = a != nullptr ? std::find (modelSlugs.begin(), modelSlugs.end(), a->selectedModel) : modelSlugs.end();
        chosenModel = at == modelSlugs.end() ? -1 : int (at - modelSlugs.begin());
    }
    choices.clear();
    for (const auto& m : f.group.members)
        if (std::none_of (choices.begin(), choices.end(), [&] (const auto& c) { return c.id == m.intent.group; }))
            choices.push_back ({ m.intent.group, text (m.intent.name) });
    if (pressMessage.isNotEmpty() && stampNow() != pressStamp)
        pressMessage.clear();

    std::vector<ChangeRow> changes;
    const auto details = proposalDifference (changes);
    std::vector<PeerRow> peers;
    const auto groupText = groupSentence (f.group, peers);
    auto sentence = todaysSentence (f);
    if (pressMessage.isNotEmpty() && ! sentence.contains (pressMessage))
        sentence += " / " + pressMessage;
    if (status.getText() != sentence)
        status.setText (sentence, juce::dontSendNotification);
    if (groupStatus.getText() != groupText)
        groupStatus.setText (groupText, juce::dontSendNotification);
    if (difference.getText() != details)
        difference.setText (details, false);

    shownPage = pageFor (f);
    const auto mcp = processor.mcpView();
    const auto shown = shownFor (f, mcp, sentence, groupText, details, std::move (changes), std::move (peers));
    if (auto* screen = view.screen.get(); screen != nullptr && screen->shown != shown)
        screen->show (shown);

    juce::String model = "NO MODEL";
    if (f.workflow.connection.phase == companion::ClientPhase::failed)
        model = "OFFLINE";
    else if (f.signedIn)
    {
        model = "CHOOSE MODEL";
        for (const auto& m : a->models)
            if (m.slug.isNotEmpty() && m.slug == a->selectedModel)
                model = (m.displayName.isEmpty() ? m.slug : m.displayName).toUpperCase();
    }
    if (auto* readout = view.readout.get(); readout != nullptr && readout->text.text != model)
        readout->show (model);

    juce::String layer, state = "signed out";
    if (f.workflow.connection.phase == companion::ClientPhase::failed)
    {
        layer = "amber";
        state = "companion offline";
    }
    else if (f.workflow.accountPending && signingIn)
        state = "signing in";
    else if (f.workflow.accountPending && a != nullptr && a->providerStatus == rack::oauth::Status::accessExpired)
        state = "renewing";
    else if (f.signedIn)
    {
        layer = "green";
        state = "connected";
    }
    else if (a != nullptr && a->providerStatus == rack::oauth::Status::accessExpired)
    {
        layer = "amber";
        state = "sign-in expired";
    }
    else if (f.workflow.connection.phase == companion::ClientPhase::connecting || (a == nullptr && f.workflow.accountPending))
        state = "connecting";
    if (auto* account = dynamic_cast<Target*> (view.findChildWithID ("ai.account")); account != nullptr && (account->litLayer() != layer || account->state() != state))
        account->show (layer, state);

    const auto& link = f.group.link;
    const juce::String linkedLayer = link.phase == GroupLinkPhase::admitted ? "lit" : "";
    const juce::String linkedState = link.phase == GroupLinkPhase::admitted ? "linked"
                                   : link.phase == GroupLinkPhase::pending  ? "link pending"
                                   : link.intent                            ? "linking"
                                                                            : "not linked";
    if (auto* linked = dynamic_cast<Target*> (view.findChildWithID ("ai.linked"));
        linked != nullptr && (linked->litLayer() != linkedLayer || linked->state() != linkedState))
        linked->show (linkedLayer, linkedState);
    mcpLight->show (mcp.enabled && mcp.connected ? "lit" : "",
                    mcp.instance.isNull() ? juce::String ("unavailable")
                    : ! mcp.enabled       ? juce::String ("access off")
                    : ! mcp.connected     ? juce::String ("access on, waiting for the MCP service")
                    : mcp.clientsAvailable ? "access on, " + clientCount (mcp)
                                           : juce::String ("access on"));

    // A running request blinks its button; every blink is in step, from the one clock.
    const bool running = f.phase == AiProposalPhase::pending || f.workflow.generationWaiting;
    const bool blink = services.nowMs() / 250 % 2 == 0;
    leds = Leds { running && ! f.workflow.refining && blink, running && f.workflow.refining && blink, f.playing,
                  f.workflow.applyPending ? blink : f.proposed && f.audioReady };
    for (const auto* id : { "ai.generate", "ai.refine", "ai.preview", "ai.apply" })
        if (auto* b = dynamic_cast<SkinButton*> (button (id)))
            b->showLit();
}
} // namespace airack::studio
