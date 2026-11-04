#include "GenerationRequest.h"
#include "LocalPreview.h"
#include "GenerationLog.h"
#include "AiCatalog.h"
#include "GroupPerformance.h"
#include "StrictJson.h"
#include <charconv>
#include <algorithm>
#include <cmath>
#include <new>
#include <memory>

namespace airack::ai
{
namespace
{
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject;
    for (const auto& field : fields) o->setProperty(field.first, field.second);
    return o;
}
juce::var finiteNumber(double value) { return std::isfinite(value) ? juce::var(value) : juce::var(); }
bool cancelledNow(const CancelCheck& cancelled) { return cancelled && cancelled(); }
bool rawTextValid(std::string_view text, std::size_t maximum)
{
    return text.size() <= maximum && text.find('\0') == std::string_view::npos
        && (text.empty() || juce::CharPointer_UTF8::isValidString(text.data(), static_cast<int>(text.size())));
}
juce::String string(std::string_view text)
{
    return juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
}
bool valid(const GenerationRequest& r)
{
    (void) parameterRegistry(); // Control-thread bootstrap for saved macro target keys.
    return !r.tag.runtime.isNull() && r.tag.requestId != 0 && validateProjectDocument(r.source)
        && r.policy.scope <= RequestScope::both && r.model.isNotEmpty() && r.model.length() <= 256
        && (!r.policy.requirePattern || r.policy.scope != RequestScope::sound)
        && r.model.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:")
        && !r.instruction.empty() && rawTextValid(r.instruction, 8192)
        && rawTextValid(r.role, 128) && rawTextValid(r.key, 128) && rawTextValid(r.scale, 128)
        && (!r.capturedPhrase || validatePatternDocument(*r.capturedPhrase))
        && (!r.capturedBpm || (std::isfinite(*r.capturedBpm) && *r.capturedBpm >= 20 && *r.capturedBpm <= 400))
        && r.namesInUse.size() <= maximumNamesInUse
        && std::all_of(r.namesInUse.begin(), r.namesInUse.end(), [](const juce::String& n) { return n.isNotEmpty() && n.getNumBytesAsUTF8() <= maximumSoundNameBytes; });
}

juce::var patternWire(const PatternDocument& p)
{
    juce::Array<juce::var> notes;
    for (std::size_t i = 0; i < p.noteCount; ++i)
    {
        const auto& n = p.notes[i];
        notes.add(juce::Array<juce::var>{n.startBeats,n.durationBeats,int(n.pitch),int(n.velocity),int(n.channel)});
    }
    return object({{"bars", static_cast<int>(p.bars)}, {"meter_numerator", static_cast<int>(p.meter.numerator)},
        {"meter_denominator", static_cast<int>(p.meter.denominator)}, {"grid", int(p.grid)},
        {"swing", p.swing}, {"notes", notes}});
}
bool linkedValid(const GenerationRequest& r, const GroupPerformanceCapture& c)
{
    const auto& intent = c.ownership.requester;
    return validateGroupPerformanceCapture(c)
        && GroupUuid(r.tag.runtime) == c.ownership.requesterRuntime
        && r.tag.editRevision == c.requesterEditRevision && r.source.group && *r.source.group == intent
        && std::string_view(r.role) == groupTextView(intent.role) && std::string_view(r.key) == groupTextView(intent.key)
        && std::string_view(r.scale) == groupTextView(intent.scale)
        && (!intent.bpm || r.capturedBpm == intent.bpm);
}
// Prompt-only numeric arrays retain binary32 precision without promoting each
// control to a 17-digit binary64 decimal. Pattern times still use their doubles.
class NormalizedValuesJSON final : public juce::DynamicObject
{
public:
    explicit NormalizedValuesJSON(const ParameterValues& source) : values(source) {}
    std::unique_ptr<juce::DynamicObject> clone() const override
    { return std::make_unique<NormalizedValuesJSON>(values); }
    void writeAsJSON(juce::OutputStream& stream, const juce::JSON::FormatOptions&) override
    {
        stream << '[';
        bool first = true;
        for (float value : values) {
            if (!first) stream << ',';
            first = false;
            // An integer -0 would lose its sign in the pinned JSON reader.
            if (value == 0 && std::signbit(value)) { stream << "-0.0"; continue; }
            char text[64];
            const auto encoded = std::to_chars(std::begin(text), std::end(text), value);
            jassert(encoded.ec == std::errc{}); // Every finite binary32 fits in 64 bytes.
            stream.write(text, static_cast<std::size_t>(encoded.ptr - text));
        }
        stream << ']';
    }
private:
    ParameterValues values;
};
juce::var normalizedValues(const ParameterValues& values)
{
    return juce::var(new NormalizedValuesJSON(values));
}
template<std::size_t N>
juce::var usedOrder(const std::array<std::uint8_t, N>& slots, std::uint8_t count)
{
    juce::Array<juce::var> result;
    for (std::size_t i = 0; i < count; ++i) result.add(static_cast<int>(slots[i]));
    return result;
}
juce::var committedWire(const ProjectDocument& d)
{
    const auto& sound = d.sound; const auto& layout = sound.layout;
    juce::Array<juce::var> synths, effects, orders, macros;
    for (auto use : layout.synthUse) synths.add(int(use));
    for (const auto& effect : layout.effects)
        effects.add(juce::Array<juce::var>{int(effect.use),int(effect.kind),int(effect.chain)});
    for (std::size_t i = 0; i < layout.effectOrder.size(); ++i) orders.add(usedOrder(layout.effectOrder[i], layout.effectCount[i]));
    for (const auto& binding : sound.macros)
    {
        const auto target = parameterIndexForKey(binding.targetKey);
        macros.add(juce::Array<juce::var>{target ? juce::var(int(*target)) : juce::var(), double(binding.amount)});
    }
    return object({{"values_normalized", normalizedValues(sound.values)},
        {"synth_use", synths}, {"synth_order", usedOrder(layout.synthOrder, layout.synthCount)},
        {"effect_slots", effects}, {"effect_order", orders}, {"macro_bindings", macros}, {"pattern", patternWire(d.pattern)},
        {"playback_mode", int(d.mode)}});
}
juce::var observationsWire(const PeerPlaybackObservation& p)
{
    const auto& c = p.controls; const auto& n = p.notes;
    // Decimal strings preserve known uint64 ages without JSON double rounding.
    const auto controls = object({{"source", "polled_controls"}, {"availability", int(c.availability)},
        {"reason", int(c.reason)}, {"age_ms", juce::String(std::to_string(c.ageMs))}});
    if (c.availability != ObservedMidiAvailability::unavailable) controls.getDynamicObject()->setProperty("values_normalized", normalizedValues(c.values));
    juce::Array<juce::var> rows;
    for (std::size_t i = 0; i < n.noteCount; ++i)
    {
        const auto& note = n.notes[i];
        rows.add(juce::Array<juce::var>{note.startBeats, note.durationBeats, static_cast<int>(note.pitch),
            static_cast<int>(note.velocity), static_cast<int>(note.channel), note.held});
    }
    const auto notes = object({{"source", "external_midi_observation"}, {"availability", int(n.availability)},
        {"reason", int(n.reason)}, {"age_ms", juce::String(std::to_string(n.ageMs))},
        {"truncated", n.truncated}, {"window_beats", n.windowBeats}, {"notes", rows}});
    if (n.meter)
    {
        notes.getDynamicObject()->setProperty("meter_numerator", static_cast<int>(n.meter->numerator));
        notes.getDynamicObject()->setProperty("meter_denominator", static_cast<int>(n.meter->denominator));
    }
    if (n.bpm) notes.getDynamicObject()->setProperty("bpm", *n.bpm);
    return object({{"controls", controls}, {"notes", notes}});
}
juce::var personalitySummaries(const SoundDocument& d)
{
    juce::Array<juce::var> rows;
    if (d.personalities) for (const auto& i : d.personalities->instances) {
        juce::Array<juce::var> units, controls;
        for (const auto& u : i.design.units) for (const auto& b : i.bindings) if (b.unit==u.id) units.add(juce::Array<juce::var>{int(u.id),int(u.module),int(b.slot)});
        for (const auto& c : i.design.panel.controls) controls.add(juce::Array<juce::var>{int(c.unit),c.key});
        rows.add(juce::Array<juce::var>{i.id.toDashedString(),i.design.id.toDashedString(),juce::String(std::to_string(i.design.revision)),i.design.name,int(i.design.role),i.edited,units,controls});
    }
    return rows;
}
juce::var parameterIds()
{
    juce::Array<juce::var> ids;
    for (const auto& p : parameterRegistry()) ids.add(p.id);
    return ids;
}
juce::var linkedWire(const GroupPerformanceCapture& c)
{
    const auto& intent = c.ownership.requester;
    juce::Array<juce::var> peers;
    for (std::size_t i = 0; i < c.peerCount; ++i)
    {
        const auto& d = c.ownership.peers[i].project; const auto& member = *d.group;
        peers.add(object({{"member_id", juce::Uuid(member.member.getRawData()).toString()}, {"role", string(groupTextView(member.role))},
            {"key", string(groupTextView(member.key))}, {"scale", string(groupTextView(member.scale))},
            {"explicit_bpm", member.bpm ? juce::var(*member.bpm) : juce::var()}, {"committed", committedWire(d)},
            {"observations", observationsWire(c.playback[i])}, {"personalities",personalitySummaries(d.sound)}}));
    }
    return object({{"version", 1}, {"group_id", juce::Uuid(intent.group.getRawData()).toString()}, {"group_name", string(groupTextView(intent.name))},
        {"requester_role", string(groupTextView(intent.role))}, {"key", string(groupTextView(intent.key))}, {"scale", string(groupTextView(intent.scale))},
        {"explicit_bpm", intent.bpm ? juce::var(*intent.bpm) : juce::var()}, {"parameter_value_order", "parameter_ids"}, {"parameter_ids", parameterIds()},
        {"observed_note_fields", juce::Array<juce::var>{"start_beats", "duration_beats", "pitch", "velocity", "channel", "held"}},
        {"availability_codes",juce::Array<juce::var>{"unavailable","fresh","stale"}},
        {"control_reason_codes",juce::Array<juce::var>{"none","not_captured","read_conflict","invalid_routing","stale_publisher","clock_rollback"}},
        {"note_reason_codes",juce::Array<juce::var>{"none","no_notes","stopped","missing_timing","invalid_timing","internal_mode","unprepared","invalid_block","input_capacity","stale_publisher","clock_rollback"}}, {"peers", peers}});
}
const char* proposalName(ProposalStatus status)
{
    switch (status)
    {
        case ProposalStatus::accepted: return "accepted";
        case ProposalStatus::malformed: return "malformed";
        case ProposalStatus::unsupported: return "unsupported";
        case ProposalStatus::scopeViolation: return "scopeViolation";
        case ProposalStatus::macroChangeNotRequested: return "macroChangeNotRequested";
        case ProposalStatus::replacementRequired: return "replacementRequired";
        case ProposalStatus::capacityReached: return "capacityReached";
        case ProposalStatus::lockedDependency: return "lockedDependency";
        case ProposalStatus::invalidCandidate: return "invalidCandidate";
        case ProposalStatus::requestedPatternMissing: return "requestedPatternMissing";
        case ProposalStatus::outOfReach: return "outOfReach";
    }
    return "invalidCandidate";
}
const char* renderName(RenderStatus status)
{
    switch (status)
    {
        case RenderStatus::ready: return "ready";
        case RenderStatus::invalidInput: return "invalidInput";
        case RenderStatus::cancelled: return "cancelled";
        case RenderStatus::nonFinite: return "nonFinite";
        case RenderStatus::clipped: return "clipped";
        case RenderStatus::renderFailure: return "renderFailure";
    }
    return "renderFailure";
}
juce::String safeProviderCode(const juce::String& code)
{
    // Only literals emitted by the accepted OAuth boundary can become diagnostics.
    // The concrete callback is also used offline, so do not trust arbitrary strings.
    const juce::StringArray known {
        "invalid_grant", "invalid_client", "invalid_scope", "access_denied", "insufficient_quota",
        "rate_limit_exceeded", "server_error", "temporarily_unavailable", "invalid_token", "model_not_found",
        "permission_denied", "refresh_too_early", "structured_model_invalid", "structured_prompt_invalid",
        "structured_schema_invalid", "structured_request_limit", "sse_size_limit", "sse_unterminated",
        "sse_done_before_completed", "sse_invalid_event_json", "sse_missing_event_type", "sse_event_type_mismatch",
        "sse_error", "sse_failed", "sse_incomplete", "sse_refusal", "sse_event_after_completed",
        "sse_invalid_delta", "sse_text_limit", "sse_invalid_done_text", "sse_done_text_mismatch",
        "sse_terminal_status", "sse_terminal_output", "sse_message_content", "sse_output_part",
        "sse_empty_output_no_delta_terminal_empty", "sse_empty_output_delta_present_terminal_message",
        "sse_empty_output_no_delta_terminal_message", "sse_empty_output_delta_present_terminal_non_message",
        "sse_empty_output_no_delta_terminal_non_message", "sse_text_mismatch", "sse_duplicate_event_field",
        "sse_invalid_line", "sse_missing_completed", "sse_pending_data"
    };
    return known.contains(code) ? code : juce::String("generation_provider_failure");
}
}

juce::var detail::measurementsForFeedback(const AudioMeasurements& m)
{
    juce::Array<juce::var> bands;
    for (double value : m.bandFractions) bands.add(finiteNumber(value));
    return object({{"finite", m.finite}, {"clipped_samples", static_cast<juce::int64>(m.clippedSamples)},
        {"peak", finiteNumber(m.peak)}, {"rms", finiteNumber(m.rms)},
        {"onset_seconds", m.onsetSeconds ? finiteNumber(*m.onsetSeconds) : juce::var()},
        {"activity_end_seconds", finiteNumber(m.activityEndSeconds)}, {"decay_seconds", finiteNumber(m.decaySeconds)},
        {"band_fractions", bands}, {"tail_active_at_end", m.tailActiveAtEnd}});
}


namespace
{
bool mergeCatalogIntent(const ProposalEdits& base, const ProposalEdits& next, ProposalEdits& output)
{
    auto combined = base;
    const auto merge = [](auto& destination, const auto& input, auto same) {
        for (const auto& row : input) {
            auto found = std::find_if(destination.begin(), destination.end(), [&](const auto& old) { return same(old, row); });
            if (found == destination.end()) destination.push_back(row); else *found = row;
        }
    };
    for (const auto& s : next.catalogSelections) {
        const auto old = std::find_if(combined.catalogSelections.begin(), combined.catalogSelections.end(), [&](const auto& v) { return v.localId == s.localId; });
        if (old != combined.catalogSelections.end() && *old != s) return false;
        if (old == combined.catalogSelections.end()) combined.catalogSelections.push_back(s);
    }
    merge(combined.parameters, next.parameters, [](const auto& a, const auto& b) { return a.id == b.id; });
    merge(combined.catalogParameters, next.catalogParameters, [](const auto& a, const auto& b) { return a.selection == b.selection && a.unit == b.unit && a.key == b.key; });
    merge(combined.macros, next.macros, [](const auto& a, const auto& b) { return a.index == b.index; });
    combined.racks.insert(combined.racks.end(), next.racks.begin(), next.racks.end());
    if (next.pattern) combined.pattern = next.pattern;
    combined.reconciliation.detach.insert(combined.reconciliation.detach.end(), next.reconciliation.detach.begin(), next.reconciliation.detach.end());
    combined.reconciliation.remove.insert(combined.reconciliation.remove.end(), next.reconciliation.remove.begin(), next.reconciliation.remove.end());
    combined.reconciliation.replace.insert(combined.reconciliation.replace.end(), next.reconciliation.replace.begin(), next.reconciliation.replace.end());
    combined.reconciliation.controls.insert(combined.reconciliation.controls.end(), next.reconciliation.controls.begin(), next.reconciliation.controls.end());
    combined.characteristics = next.characteristics; combined.explanation = next.explanation;
    if (next.name.isNotEmpty()) combined.name = next.name;
    output = std::move(combined); return true;
}
bool definitionsPresent(const ProposalEdits& edits, const CatalogAuthority& authority)
{
    return std::all_of(edits.catalogSelections.begin(), edits.catalogSelections.end(), [&](const auto& s) {
        return std::any_of(authority.definitions.begin(), authority.definitions.end(), [&](const auto& d) { return d.design == s.design && d.revision == s.revision && d.contentSha256 == s.contentSha256; });
    });
}
}

namespace {
GenerationResult generateProposalImpl(const GenerationRequest& request,const StructuredCall& call,const CancelCheck& cancelled,
    const GroupPerformanceCapture* linked,const LocalPreviewEvaluator* evaluateLocal,std::optional<PreviewReceipt>* retainedReceipt,const LocalPreviewContext* localContext)
{
    GenerationResult result;
    result.tag = request.tag;
    // Materialization retains the already-spent provider budget on every terminal.
    if(request.catalog&&request.catalog->phase==CatalogPhase::materialize){const auto& c=*request.catalog;
        result.inferenceCalls=c.inferenceCallsUsed;result.providerStatus=c.providerStatus;result.httpStatus=c.httpStatus;}
    const auto began=std::chrono::steady_clock::now();
    std::uint64_t outputBytes=0;ProposalDecodeReason decodeReason=ProposalDecodeReason::accepted;
    auto log=[&](diagnostics::Stage stage) noexcept {
        try{diagnostics::Event event;event.component=diagnostics::Component::companion;event.stage=stage;event.runtime=request.tag.runtime;
            event.requestId=request.tag.requestId;event.model=request.model;event.generationStatus=int(result.status);event.providerStatus=int(result.providerStatus);
            event.proposalStatus=int(result.proposalStatus);event.renderStatus=int(result.renderStatus);event.httpStatus=result.httpStatus;event.calls=result.inferenceCalls;event.errorCode=result.errorCode;
            event.outputBytes=outputBytes;if(stage==diagnostics::Stage::parse)event.errorCode=proposalDecodeReasonName(decodeReason);
            event.elapsedMs=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count());diagnostics::write(event);}catch(...){}
    };
    auto finish=[&] {if(retainedReceipt&&result.status!=GenerationStatus::ready)retainedReceipt->reset();log(diagnostics::Stage::terminal);return std::move(result);};
    auto cancel = [&] { result.status=GenerationStatus::cancelled;result.candidate.reset();result.edits={};result.audio={};result.catalog.reset();return finish(); };
    const auto rendering=evaluateLocal?CatalogRendering::ownedLocal:CatalogRendering::companionPcm;
    const auto evaluate=[&](const ProjectDocument& candidate,const ProposalEdits& edits,const AuditionContext& context,const SourceAssignmentTrace* trace){
        if(!evaluateLocal)return renderPreview(candidate,context,cancelled);
        const auto receipt=(*evaluateLocal)(candidate,edits,context,*trace,cancelled);
        RenderResult rendered;
        if(!validPreviewReceipt(receipt)){rendered.status=RenderStatus::invalidInput;return rendered;}
        rendered.status=receipt.status;rendered.measurements=receipt.measurements;
        if(receipt.failure==PreviewFailure::stale||receipt.failure==PreviewFailure::cancelled)rendered.status=RenderStatus::cancelled;
        else if(receipt.failure!=PreviewFailure::none){rendered.status=RenderStatus::renderFailure;
            result.errorCode=receipt.failure==PreviewFailure::busy?"generation_local_busy":receipt.failure==PreviewFailure::budget?"generation_local_budget":receipt.failure==PreviewFailure::allocation?"generation_allocation_failure":"generation_local_source_unavailable";}
        if(rendered.status==RenderStatus::ready&&retainedReceipt)*retainedReceipt=receipt;
        return rendered;
    };
    log(diagnostics::Stage::capture);
    try
    {
        if (!linked && cancelledNow(cancelled)) return cancel();
        if(request.catalog&&request.catalog->authority&&request.catalog->authority->descriptorJson.size()>maximumCatalogDescriptorBytes){result.errorCode="generation_catalog_descriptor_limit";return finish();}
        if ((!call && (!request.catalog || request.catalog->phase != CatalogPhase::materialize)) || !valid(request)
            || !detail::catalogRequestValid(request) || (request.catalog&&request.catalog->authority->rendering!=rendering) || (linked && !linkedValid(request, *linked))) return finish();
        const auto capturedStorage = std::make_unique<const GenerationRequest>(request);
        const auto& captured = *capturedStorage;
        const auto capturedPeers = linked ? std::make_unique<const GroupPerformanceCapture>(*linked) : nullptr;
        if (linked && cancelledNow(cancelled)) return cancel();
        const auto contextStorage=std::unique_ptr<AuditionContext>(new AuditionContext(previewContextFor(captured,{},localContext?&localContext->sourceMap:nullptr,localContext?&captured.source.sound:nullptr)));
        const auto& context=*contextStorage;
        if (captured.catalog && captured.catalog->phase == CatalogPhase::materialize) {
            const auto& c = *captured.catalog;
            result.inferenceCalls = c.inferenceCallsUsed; result.providerStatus = c.providerStatus; result.httpStatus = c.httpStatus;
            auto candidateStorage=std::make_unique<ProjectDocument>();auto& candidate=*candidateStorage;
            auto trace=evaluateLocal?std::make_unique<SourceAssignmentTrace>():nullptr;
            result.proposalStatus = admitProposal(captured.source,captured.policy,c.baseIntent,*c.authority,candidate,trace.get());
            log(diagnostics::Stage::admission);
            if (result.proposalStatus != ProposalStatus::accepted) {
                result.errorCode="generation_catalog_admission";
                for(const auto& definition:c.authority->definitions){RackDesign design;if(decodeRackDesign(definition.canonical.getData(),definition.canonical.getSize(),design,RackValidationMode::instanceRecall)&&!validateRackDesign(design,RackValidationMode::portableDesign)){
                    const bool embedded=captured.source.sound.personalities&&std::any_of(captured.source.sound.personalities->instances.begin(),captured.source.sound.personalities->instances.end(),[&](const auto& rack){return rack.design==design;});
                    if(!embedded)result.errorCode="generation_catalog_requires_local_native_authority";
                }}return finish();
            }
            auto staged=localContext?std::make_unique<PreviewSampleMap>():nullptr;
                if(localContext&&(!trace||!stagePreviewMap(localContext->sourceMap,*trace,*staged))){result.status=GenerationStatus::renderFailure;return finish();}
                const auto renderContext=std::unique_ptr<AuditionContext>(new AuditionContext(previewContextFor(captured, c.baseIntent.pattern,staged.get(),localContext?&candidate.sound:nullptr)));
                auto renderedStorage=std::unique_ptr<RenderResult>(new RenderResult(evaluate(candidate,c.baseIntent,*renderContext,trace.get())));auto& rendered=*renderedStorage;
            result.renderStatus = rendered.status; result.measurements = rendered.measurements; log(diagnostics::Stage::render);
            if (cancelledNow(cancelled) || rendered.status == RenderStatus::cancelled) return cancel();
            if (rendered.status != RenderStatus::ready) { result.status = GenerationStatus::renderFailure; if(result.errorCode.isEmpty())result.errorCode = "generation_render_failure"; return finish(); }
            result.status = GenerationStatus::ready; result.edits = c.baseIntent; result.candidate = std::move(candidate); result.audio = std::move(rendered.audio);
            result.catalog = CatalogResultBinding{c.root, c.authority->snapshot}; return finish();
        }
        juce::var catalog;
        // Requests without catalog authority expose no selectable recipes.
        RackCatalogSnapshot empty;
        if (!(captured.catalog?providerCatalogFor(captured.source,*captured.catalog->authority,rendering,catalog)
            :captureRackCatalog({},{},{},empty)&&providerCatalogFor(captured.source,empty,rendering,catalog))) {
            result.errorCode="generation_catalog_descriptor_limit";return finish();
        }
        const auto schema = captured.catalog ? proposalSchema(captured.policy, *captured.catalog->authority) : proposalSchema(captured.policy);
        const char* scope = captured.policy.scope == RequestScope::sound ? "sound" : captured.policy.scope == RequestScope::pattern ? "pattern" : "both";
        const auto payload = object({{"version", 1}, {"instruction", string(captured.instruction)}, {"catalog", catalog},
            {"policy", object({{"scope", scope}, {"allow_macro_changes", captured.policy.allowMacroChanges}, {"allow_slot_reuse", captured.policy.allowSlotReuse}, {"require_pattern", captured.policy.requirePattern}})},
            {"context", object({{"role", string(captured.role)}, {"key", string(captured.key)}, {"scale", string(captured.scale)},
                {"phrase", patternWire(context.phrase)}, {"saved_pattern",patternWire(captured.source.pattern)}, {"bpm", context.bpm},
                {"phrase_source", context.phraseSource == PhraseSource::defaultNotes ? "default_notes" : "captured"},
                {"tempo_source", context.tempoSource == TempoSource::defaultTempo ? "default_tempo" : "captured"}})},
            {"proposal_schema", schema},
            {"output_rules", "Return one JSON object conforming to proposal_schema. Pattern null preserves saved notes; a pattern object replaces editable notes; empty notes explicitly clear saved notes. When policy.require_pattern is true, return a pattern object with at least one note; null and empty notes are rejected even when saved notes exist. Positions and durations use quarter-note beats; MIDI pitch, velocity and channel use bounded plain JSON integer tokens. default_notes are audition-only. When the instruction requests a phrase and policy permits pattern edits, provide the phrase in pattern instead of directions in explanation. Use plain JSON integer tokens. Use supported catalog controls and modules, preserve locked dependencies and the requested scope, retain macro assignments and retired slots unless policy permits changes. Repair the literal proposal/render rejection within those same bounds; for clipped or nonFinite audio choose supported finite, lower-level edits. No similarity score is supplied. catalog.controls is the user's reach: the house unit and each present bank's rack. Change only those, or, on a rack this proposal places, the keys its module lists in catalog.module_keys, through catalog_parameter_edits; a synth's engine follows its rack. Macro targets are continuous controls in that reach. To remove a placed rack, name its instance in reconciliation.remove; to swap one, remove it and select the new rack at its position. Rack edits only move placed racks, a rack's banks together, or remove the banks of a rack named for removal; any other rack edit, and reconciliation.detach, is rejected as outOfReach. When the instruction describes a different sound rather than a change to the current one, choose the library racks that fit it best, removing or swapping placed racks that do not; a request for a change to the current sound keeps its racks. Name the sound in name with 2 to 4 real, descriptive words about its role and character, like \"Ethereal Rolling Bass\", differing from every entry in names_in_use."}, {"repair", {}}});
        payload.getDynamicObject()->setProperty("output_rules", payload["output_rules"].toString()
            + " Preserve captured output.gain_db unless the user requests a level change or clipping repair requires it. This is the instrument's calibrated final gain: +30 dB is the default; 0 dB lowers that default by 30 dB, not a neutral reset. Shape loudness with the selected rack's level controls while retaining appropriate headroom.");
        juce::Array<juce::var> names;
        for (const auto& n : captured.namesInUse) names.add(n);
        payload.getDynamicObject()->setProperty("names_in_use", names);
        payload.getDynamicObject()->setProperty("personalities",personalitySummaries(captured.source.sound));
        payload.getDynamicObject()->setProperty("personality_columns",juce::Array<juce::var>{"instance","design","revision","name","role","edited","units_local_module_slot","controls_local_key"});
        if (captured.catalog) {
            juce::Array<juce::var> definitions;
            for (const auto& d : captured.catalog->authority->definitions) {
                juce::var value;
                if (!parseStrictJson(std::string_view(static_cast<const char*>(d.canonical.getData()), d.canonical.getSize()), value, 65536)) return finish();
                definitions.add(value);
            }
            const auto& base = captured.catalog->baseIntent;
            juce::Array<juce::var> selections, semanticParameters, parameters, racks, macros, detach, remove, replace, controls;
            for (const auto& s : base.catalogSelections)
                selections.add(object({{"local_id", int(s.localId)}, {"design", s.design.toDashedString()}, {"revision", juce::String(std::to_string(s.revision))},
                    {"content_sha256", s.contentSha256}, {"destination", object({{"role", s.destination.role == RackRole::synth ? "synth" : "fx"}, {"chain", int(s.destination.chain)}, {"position", int(s.destination.position)}})}}));
            for (const auto& p : base.catalogParameters)
                semanticParameters.add(object({{"selection", int(p.selection)}, {"unit", int(p.unit)}, {"key", p.key}, {"normalized", double(p.normalized)}}));
            for (const auto& p : base.parameters)
                parameters.add(object({{"id", p.id}, {"normalized", double(p.normalized)}}));
            const auto rackKinds = schema["properties"]["rack_edits"]["items"]["properties"]["kind"]["enum"];
            const auto effectKinds = schema["properties"]["rack_edits"]["items"]["properties"]["effect"]["enum"];
            for (const auto& r : base.racks)
                racks.add(object({{"kind", rackKinds[int(r.kind)]}, {"slot", int(r.slot)}, {"chain", int(r.destination)},
                    {"position", int(r.position)}, {"effect", effectKinds[int(r.effect)]}}));
            for (const auto& m : base.macros)
                macros.add(object({{"index", int(m.index)}, {"target_id", m.targetId.isEmpty() ? juce::var() : juce::var(m.targetId)}, {"amount", double(m.amount)}}));
            for (const auto& id : base.reconciliation.detach) detach.add(id.toDashedString());
            for (const auto& id : base.reconciliation.remove) remove.add(id.toDashedString());
            for (const auto& id : base.reconciliation.replace) replace.add(id.toDashedString());
            constexpr const char* widgets[]{"rotary", "verticalSlider", "toggle", "choice"};
            constexpr const char* responses[]{"linear", "logarithmic"};
            for (const auto& r : base.reconciliation.controls) {
                juce::var replacement;
                if (r.replacement) { const auto& c = *r.replacement;
                    if (c.widget > FocusWidget::choice || c.response > FocusResponse::logarithmic) return finish();
                    replacement = object({{"label", c.label}, {"unit", int(c.unit)}, {"section", int(c.section)}, {"key", c.key},
                        {"widget", widgets[std::size_t(c.widget)]}, {"response", responses[std::size_t(c.response)]},
                        {"minimum", double(c.minimum)}, {"maximum", double(c.maximum)}});
                }
                controls.add(object({{"instance", r.instance.toDashedString()}, {"control", int(r.control)}, {"replacement", replacement}}));
            }
            payload.getDynamicObject()->setProperty("selected_definitions", definitions);
            payload.getDynamicObject()->setProperty("base_catalog_intent", object({{"parameter_edits", parameters}, {"rack_edits", racks},
                {"macro_edits", macros}, {"pattern", base.pattern ? patternWire(*base.pattern) : juce::var()},
                {"reconciliation", object({{"detach", detach}, {"remove", remove}, {"replace", replace}, {"controls", controls}})},
                {"characteristics", base.characteristics}, {"explanation", base.explanation}, {"name", base.name},
                {"catalog_selections", selections}, {"catalog_parameter_edits", semanticParameters}}));
            payload.getDynamicObject()->setProperty("output_rules", payload["output_rules"].toString()
                + " Select exact catalog identities/revisions/hashes using unique local_id values. Destinations: a synth selection's destination chain is always 4 (the master chain) and its position 0 to 3 picks the synth slot, with 0 for the first synth; an effect selection's chain is 0 to 3 to sit after that one synth or 4 for the master output, and its position counts from 0 along that chain. Repeated designs use independent local IDs. Semantic parameter/macro targets use local selection/unit keys, not guessed physical banks. Preserve existing base selection identities/destinations; refinements add unused IDs and change semantic values. Complete base_catalog_intent retains all previously accepted edits; null pattern preserves its prior pattern intent, and an explicit empty pattern remains a replacement. Definitions are native captured grounding, not provider permissions. All required catalog arrays remain present.");
        }
        if (juce::JSON::toString(schema,true,17).getNumBytesAsUTF8()>rack::oauth::maximumStructuredSchemaBytes) return finish();
        if (capturedPeers)
        {
            payload.getDynamicObject()->setProperty("linked_context", linkedWire(*capturedPeers));
            payload.getDynamicObject()->setProperty("output_rules", payload["output_rules"].toString()
                + " Respect explicit key, scale and roles. Propose register, rhythm, envelope, spectral emphasis, stereo and effect density relative to selected peers; leave useful space. Label unavailable observations and use committed patterns when relevant. Do not infer unheard clips or audible guarantees. Only the requesting instance may receive supported edits through proposal_schema; peers are context and never edit targets.");
        }
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            if (cancelledNow(cancelled)) return cancel();
            std::size_t promptBytes;
            if (!rackCatalogEscapedSize(payload, rack::oauth::maximumStructuredPromptBytes, promptBytes))
            {
                result.status = GenerationStatus::invalidRequest; result.errorCode = "generation_prompt_limit"; return finish();
            }
            const auto prompt = juce::JSON::toString(payload, true, captured.catalog ? 22 : 17).toStdString();
            if (prompt.size() > rack::oauth::maximumStructuredPromptBytes)
            {
                result.status = GenerationStatus::invalidRequest; result.errorCode = "generation_prompt_limit"; return finish();
            }
            rack::oauth::StructuredResult response;
            ++result.inferenceCalls;
            try { response = call(captured.model, prompt, schema); }
            catch (...)
            {
                if (cancelledNow(cancelled)) return cancel();
                result.status = GenerationStatus::providerFailure; result.errorCode = "generation_provider_exception"; return finish();
            }
            if (cancelledNow(cancelled) || response.result.status == rack::oauth::Status::cancelled) return cancel();
            result.providerStatus = response.result.status; result.httpStatus = response.result.httpStatus;
            outputBytes=response.text.getNumBytesAsUTF8();
            log(diagnostics::Stage::provider);
            if (response.result.status != rack::oauth::Status::connected || response.text.isEmpty()
                || response.text.getNumBytesAsUTF8() > rack::oauth::maximumStructuredOutputBytes)
            {
                result.status = GenerationStatus::providerFailure;
                result.errorCode = safeProviderCode(response.result.errorCode); return finish();
            }
            const auto text = response.text.toStdString();
            auto editsStorage=std::make_unique<ProposalEdits>();auto& edits=*editsStorage;
            auto candidateStorage=std::make_unique<ProjectDocument>();auto& candidate=*candidateStorage;
            result.proposalStatus = ProposalStatus::malformed;
            result.renderStatus = RenderStatus::invalidInput; result.measurements = {};
            // The captured request chooses the schema; provider omissions cannot
            // downgrade authority. Context-free requests retain legacy decoding.
            bool decoded = captured.catalog ? decodeProposal(text, captured.policy, *captured.catalog->authority, edits, &decodeReason)
                : decodeProposal(text, captured.policy, edits, &decodeReason);
            if (decoded && captured.catalog) {
                auto combinedStorage=std::make_unique<ProposalEdits>();auto& combined=*combinedStorage;
                decoded = mergeCatalogIntent(captured.catalog->baseIntent, edits, combined)
                    && detail::catalogIntentValid(combined, captured.policy, *captured.catalog->authority);
                if (decoded) edits = std::move(combined);
                if (decoded && !definitionsPresent(edits, *captured.catalog->authority)) {
                    // A selection carries its rack edits to materialize; one outside reach is repaired here.
                    if (!detail::rackEditsInReach(captured.source.sound, edits)) { decoded = false; result.proposalStatus = ProposalStatus::outOfReach; }
                    else {
                        if (cancelledNow(cancelled)) return cancel();
                        result.status = GenerationStatus::catalogSelectionRequired; result.proposalStatus = ProposalStatus::accepted; result.edits = std::move(edits);
                        result.catalog = CatalogResultBinding{captured.catalog->root, captured.catalog->authority->snapshot}; return finish();
                    }
                }
            }
            log(diagnostics::Stage::parse);
            auto trace=evaluateLocal?std::make_unique<SourceAssignmentTrace>():nullptr;
            if(decoded)result.proposalStatus=captured.catalog
                ? admitProposal(captured.source,captured.policy,edits,*captured.catalog->authority,candidate,trace.get())
                : admitProposal(captured.source,captured.policy,edits,candidate,trace.get());
            log(diagnostics::Stage::admission);
            if (cancelledNow(cancelled)) return cancel();
            if (result.proposalStatus == ProposalStatus::accepted)
            {
                auto staged=localContext?std::make_unique<PreviewSampleMap>():nullptr;
                if(localContext&&(!trace||!stagePreviewMap(localContext->sourceMap,*trace,*staged))){result.status=GenerationStatus::renderFailure;return finish();}
                const auto renderContext=std::unique_ptr<AuditionContext>(new AuditionContext(previewContextFor(captured, edits.pattern,staged.get(),localContext?&candidate.sound:nullptr)));
                auto renderedStorage=std::unique_ptr<RenderResult>(new RenderResult(evaluate(candidate,edits,*renderContext,trace.get())));auto& rendered=*renderedStorage;
                result.renderStatus = rendered.status; result.measurements = rendered.measurements;
                log(diagnostics::Stage::render);
                if (cancelledNow(cancelled) || rendered.status == RenderStatus::cancelled) return cancel();
                if (rendered.status == RenderStatus::ready)
                {
                    // The last cancellation guard precedes every published field.
                    result.status = GenerationStatus::ready;
                    result.candidate = std::move(candidate); result.edits = std::move(edits);
                    if (captured.catalog) result.catalog = CatalogResultBinding{captured.catalog->root, captured.catalog->authority->snapshot};
                    result.audio = std::move(rendered.audio); return finish();
                }
                if (rendered.status != RenderStatus::clipped && rendered.status != RenderStatus::nonFinite)
                {
                    result.status = GenerationStatus::renderFailure; if(result.errorCode.isEmpty())result.errorCode = "generation_render_failure"; return finish();
                }
            }
            if (attempt == 2) break;
            const auto repair = object({{"previous_response", response.text},
                {"proposal_status", proposalName(result.proposalStatus)}, {"render_status", renderName(result.renderStatus)},
                {"measurements", result.renderStatus == RenderStatus::clipped || result.renderStatus == RenderStatus::nonFinite
                    ? detail::measurementsForFeedback(result.measurements) : juce::var()}});
            if (juce::JSON::toString(repair,true,17).getNumBytesAsUTF8()>65536) {
                result.status=GenerationStatus::invalidRequest;result.errorCode="generation_repair_limit";return finish();
            }
            payload.getDynamicObject()->setProperty("repair",repair);
        }
        result.status = GenerationStatus::budgetExhausted;
        if (result.proposalStatus == ProposalStatus::requestedPatternMissing) result.errorCode = "requested_pattern_missing";
    }
    catch (const std::bad_alloc&)
    {
        result.candidate.reset(); result.edits={}; result.audio={};
        result.status = GenerationStatus::renderFailure; result.errorCode = "generation_allocation_failure";
    }
    return finish();
}
}
GenerationResult generateProposal(const GenerationRequest& request,const StructuredCall& call,const CancelCheck& cancelled,const GroupPerformanceCapture* linked){
    return generateProposalImpl(request,call,cancelled,linked,nullptr,nullptr,nullptr);
}
LocalGenerationResult generateLocalProposal(const LocalGenerationRequest& input,const StructuredCall& call,const LocalPreviewEvaluator& evaluator,const CancelCheck& cancelled,const GroupPerformanceCapture* linked){
    LocalGenerationResult result;result.numeric.tag=input.request.tag;
    if(!evaluator||!validLocalPreviewContext(input))return result;
    result.numeric=generateProposalImpl(input.request,call,cancelled,linked,&evaluator,&result.rendered,&input.local);
    if(result.numeric.status!=GenerationStatus::ready)result.rendered.reset();return result;
}
}
