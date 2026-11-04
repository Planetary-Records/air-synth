#include "FrozenRackExamples.h"
#include "FactorySounds.h"
#include "RackDesign.h"
#include "RackLibrary.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <climits>
#include <new>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "RoutedSynthEngine.h"
#include "SoundDocument.h"
#include "SoundState.h"
#include <juce_cryptography/juce_cryptography.h>
#include "ParameterRegistry.h"
#include "StrictJson.h"
#include "UserReach.h"
#include <bit>
#include <bitset>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

using namespace airack;
namespace {
int checks = 0, failures = 0, sections = 0; const char* section = "";
void check(bool pass, const char* expression, int line) {
    ++checks; if (!pass) { ++failures; std::cerr << section << ':' << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void begin(const char* s) { section = s; ++sections; std::cout << s << '\n'; }
RackDesign good() {
    RackDesign d; d.id = juce::Uuid("12345678-1234-1234-1234-123456789abc"); d.name = "Warm"; d.intent = "Original native instrument";
    d.units = {{0, NativeModule::analog, {{0, "analog.filter.cutoff", .5f}}}};
    d.panel.sections = {"Tone"};
    d.panel.controls = {{"Brightness", 0, 0, "analog.filter.cutoff", FocusWidget::rotary, FocusResponse::logarithmic, 20, 20000}};
    return d;
}
std::string json(const juce::var& v) { return juce::JSON::toString(v, true).toStdString(); }
juce::var wire(const RackDesign& d = good()) { juce::var v; CHECK(encodeRackDesignObject(d, v, RackValidationMode::portableDesign)); return v; }
void bad(const RackDesign& d) {
    CHECK(!validateRackDesign(d, RackValidationMode::portableDesign));
    juce::MemoryBlock output("keep", 4), before = output;
    CHECK(!encodeRackDesign(d, output)); CHECK(output == before);
    juce::var v = "keep"; CHECK(!encodeRackDesignObject(d, v, RackValidationMode::portableDesign)); CHECK(v == juce::var("keep"));
}
void badJson(const std::string& s) {
    auto out = good(), before = out; CHECK(!decodeRackDesign(s.data(), s.size(), out)); CHECK(out == before);
}
void badObject(const juce::var& v) {
    auto out = good(), before = out; CHECK(!decodeRackDesignObject(v, out, RackValidationMode::portableDesign)); CHECK(out == before); badJson(json(v));
}

void drum_module_and_compatibility() {
    begin("drum_module_and_compatibility");
    KernelCompatibility c;const auto before=c;CHECK(isSourceModule(NativeModule::drum)&&!isEffectModule(NativeModule::drum));CHECK(isEffectModule(NativeModule::eraReverb));
    for(std::uint8_t bank=0;bank<4;++bank){CHECK(rackAlgorithmRevision(c,NativeModule::drum,bank)==1);CHECK(setRackAlgorithmRevision(c,NativeModule::drum,bank,1));CHECK(c==before);CHECK(!setRackAlgorithmRevision(c,NativeModule::drum,bank,2)&&!setRackAlgorithmRevision(c,NativeModule::drum,bank,3));}
    CHECK(!setRackAlgorithmRevision(c,NativeModule::drum,4,1));CHECK(rackAlgorithmRevision(c,NativeModule::drum,4)==0);CHECK(!rackParameterIndex(NativeModule::drum,4,"level"));
    for(const auto invalid:{NativeModule(22),NativeModule(255)}){CHECK(!isSourceModule(invalid)&&!isEffectModule(invalid)&&rackUnitParameterCapacity(invalid)==0);CHECK(rackAlgorithmRevision(c,invalid,0)==0&&!setRackAlgorithmRevision(c,invalid,0,1));CHECK(!rackParameterIndex(invalid,0,"level"));}
    auto raw=std::make_unique<SoundAudioState>();CHECK(sourceModule(*raw,4)==NativeModule(255));raw->values[13]=1;raw->values[drumModeParameterStart]=1;CHECK(sourceModule(*raw,0)==NativeModule::drum);
    CHECK(c==before&&sizeof(c.synth)+sizeof(c.effects)==160&&sizeof(c.synth)+sizeof(c.effects)+8==168);CHECK(unsigned(NativeModule::eraReverb)==19&&unsigned(EffectKind::eraReverb)==17);
    for(std::uint8_t bank=0;bank<8;++bank){CHECK(unitReach(NativeModule::eraReverb,bank,1).size()==16);CHECK(rackAlgorithmRevision(c,NativeModule::eraReverb,bank)==before.effects[bank][17]);}
    auto d=good();d.units[0].module=NativeModule::drum;d.units[0].defaults={{0,"drum.pad.1.gain_db",.5f}};d.panel.controls.clear();CHECK(validateRackDesign(d,RackValidationMode::portableDesign));d.units[0].defaults.push_back({0,"drum_mode",0});CHECK(!validateRackDesign(d,RackValidationMode::instanceRecall));
    d=good();d.units[0].defaults={{0,"drum.pad.1.gain_db",.5f}};CHECK(!validateRackDesign(d,RackValidationMode::portableDesign));CHECK(validateRackDesign(d,RackValidationMode::instanceRecall));
    d.units[0].module=NativeModule::drum;d.units[0].defaults={{0,"engine",1}};d.panel.controls={{"Remembered engine",0,0,"engine",FocusWidget::choice,FocusResponse::linear,0,1}};CHECK(validateRackDesign(d,RackValidationMode::instanceRecall));CHECK(!validateRackDesign(d,RackValidationMode::portableDesign));
    const auto reach=unitReach(NativeModule::drum,0,1);CHECK(std::find(reach.begin(),reach.end(),13)==reach.end()&&std::find(reach.begin(),reach.end(),drumModeParameterStart)==reach.end());
}
void utf8_and_recipe_bounds() {
    begin("utf8_and_recipe_bounds");
    auto d = good(); const auto multi = juce::String::fromUTF8("\xc3\xa9");
    d.name = juce::String::repeatedString(multi, 32); d.intent = juce::String::repeatedString(multi, 256); d.panel.sections[0] = d.name; d.panel.controls[0].label = d.name;
    CHECK(validateRackDesign(d, RackValidationMode::portableDesign)); juce::MemoryBlock b; CHECK(encodeRackDesign(d, b)); CHECK(b.getSize() <= 65536);
    RackDesign out; CHECK(decodeRackDesign(b.getData(), b.getSize(), out)); CHECK(out == d);
    for (int field = 0; field < 4; ++field) { auto x = d; if (field == 0) x.name += "x"; if (field == 1) x.intent += "x"; if (field == 2) x.panel.sections[0] += "x"; if (field == 3) x.panel.controls[0].label += "x"; bad(x); }
    auto s = json(wire()); auto pos = s.find("Warm"); s.replace(pos, 4, "Wa\\u0000rm"); badJson(s);
    s = json(wire()); pos = s.find("Warm"); s.replace(pos, 4, std::string("\xc0\xaf", 2)); badJson(s);
    s = json(wire()); pos = s.find("Warm"); s.replace(pos, 4, "\\ud800"); badJson(s);
    s = json(wire()); pos = s.find("Warm"); s.replace(pos, 4, "\\udc00"); badJson(s);
    s = json(wire()); s.insert(1, "\"version\":1,"); badJson(s);
    s = json(wire()); s.resize(65536, ' '); CHECK(decodeRackDesign(s.data(), s.size(), out)); s += ' '; badJson(s);
    auto x = good(); x.units.clear(); bad(x);
    x = good(); x.units.push_back(x.units[0]); bad(x);
    x = good(); x.units.push_back({1, NativeModule::wavetable, {}}); bad(x);
    x = good(); x.units[0].id = 9; bad(x);
    x = good(); x.units[0].defaults.push_back(x.units[0].defaults[0]); bad(x);
    x = good(); x.units[0].defaults[0].unit = 1; bad(x);
    x = good(); x.units[0].module = static_cast<NativeModule>(6); bad(x);
    x = good(); x.role = static_cast<RackRole>(2); bad(x);
    x = good(); x.revision = 0; bad(x);
    x = good(); x.id = juce::Uuid::null(); bad(x);
    x = good(); x.panel.sections.resize(5, "Tone"); bad(x);
    x = good(); x.panel.sections.clear(); bad(x);
    x = good(); x.panel.controls.resize(9, x.panel.controls[0]); bad(x);
    x = good(); x.panel.controls.push_back(x.panel.controls[0]); bad(x);
    x = good(); x.panel.controls[0].section = 1; bad(x);
    x = good(); x.panel.controls[0].unit = 1; bad(x);
    for (const auto* key : {"unknown", "synth.1.level", "output.gain_db", "macro.1", "global.master_level"}) {
        x = good(); x.panel.controls[0].key = key; bad(x); x = good(); x.units[0].defaults[0].key = key; bad(x);
    }
    for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.f, 1.1f}) { x = good(); x.units[0].defaults[0].normalized = value; bad(x); }
    for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 19.f, 20001.f}) { x = good(); if (value == 20001.f) x.panel.controls[0].maximum = value; else x.panel.controls[0].minimum = value; bad(x); }
    x = good(); x.panel.controls[0].maximum = 20; bad(x);
    x = good(); x.panel.controls[0].key = "pan"; x.panel.controls[0].minimum = -1; x.panel.controls[0].maximum = 1; bad(x);
    x = good(); x.panel.controls[0].widget = FocusWidget::choice; bad(x);
    x = good(); x.panel.controls[0] = {"Mode", 0, 0, "analog.filter.mode", FocusWidget::choice, FocusResponse::linear, 0, 2}; CHECK(validateRackDesign(x, RackValidationMode::portableDesign));
    x.panel.controls[0].minimum = .5f; bad(x);
    x = good(); x.panel.controls[0] = {"Mute", 0, 0, "mute", FocusWidget::toggle, FocusResponse::linear, 0, 1}; CHECK(validateRackDesign(x, RackValidationMode::portableDesign));
    x.units[0].defaults = {{0, "engine", 1}}; bad(x);
    x = good(); for (int i = 1; i <= 8; ++i) x.units.push_back({static_cast<std::uint8_t>(i), NativeModule::drive, {}}); CHECK(validateRackDesign(x, RackValidationMode::portableDesign)); x.units.push_back({8, NativeModule::delay, {}}); bad(x);
    x = good(); x.role = RackRole::fx; x.units = {{0, NativeModule::drive, {}}}; x.panel.controls.clear(); CHECK(validateRackDesign(x, RackValidationMode::portableDesign));
    for (int i = 1; i < 8; ++i) x.units.push_back({static_cast<std::uint8_t>(i), NativeModule::delay, {}}); CHECK(validateRackDesign(x, RackValidationMode::portableDesign)); x.units.push_back({8, NativeModule::delay, {}}); bad(x);
    for (const auto* field : {"bindings", "locks", "macros", "pattern", "group", "extra"}) { auto v = wire(); v.getDynamicObject()->setProperty(field, 1); badObject(v); }
    for (auto pair : {std::pair{"version", juce::var(3)}, {"role", juce::var("unknown")}, {"id", juce::var("garbage")}}) { auto v = wire(); v.getDynamicObject()->setProperty(pair.first, pair.second); badObject(v); }
    auto v = wire(); v["units"][0].getDynamicObject()->setProperty("module", "unknown"); badObject(v);
    v = wire(); v["panel"].getDynamicObject()->setProperty("layout", "unknown"); badObject(v);
    v = wire(); v["units"][0].getDynamicObject()->setProperty("extra", true); badObject(v);
    v = wire(); v["panel"]["controls"][0].getDynamicObject()->setProperty("extra", true); badObject(v);
    v = wire(); v["units"][0]["defaults"][0].getDynamicObject()->setProperty("normalized", 1.000000001); badObject(v);
    v = wire(); v["units"][0]["defaults"][0].getDynamicObject()->setProperty("normalized", -1e-300); badObject(v);
    v = wire(); v["panel"]["controls"][0].getDynamicObject()->setProperty("minimum", 19.999999999); badObject(v);
    v = wire(); v["panel"]["controls"][0].getDynamicObject()->setProperty("maximum", 20000.000000001); badObject(v);
    v = wire(); v["panel"].getDynamicObject()->setProperty("accent", juce::String::repeatedString("x", 65536)); badObject(v);
    v = wire(); v["panel"].getDynamicObject()->setProperty("surface", "future"); CHECK(decodeRackDesignObject(v, out, RackValidationMode::portableDesign)); CHECK(out.panel.surface == RackPanel{}.surface); CHECK(out.units == good().units); CHECK(out.panel.controls == good().panel.controls);
    for (const auto* field : {"surface", "accent", "frame", "emphasis", "spacing", "density"}) {
        v = wire(); v["panel"].getDynamicObject()->removeProperty(field); CHECK(decodeRackDesignObject(v, out, RackValidationMode::portableDesign)); CHECK(out == good());
    }
    for (const auto* field : {"version", "id", "revision", "name", "intent", "role", "units", "panel"}) { v = wire(); v.getDynamicObject()->removeProperty(field); badObject(v); }
    v = wire(); v["units"][0]["defaults"][0].getDynamicObject()->setProperty("extra", 1); badObject(v);
    v = wire(); v["panel"]["controls"][0].getDynamicObject()->setProperty("widget", "unknown"); badObject(v);
    v = wire(); v["panel"]["controls"][0].getDynamicObject()->setProperty("response", "unknown"); badObject(v);
    v = wire(); v["panel"].getDynamicObject()->setProperty("extra", 1); badObject(v);
    // Depth and truncation checks use malicious metadata in an optional style.
    s = json(wire()); pos = s.find("\"light\""); s.replace(pos, 7, std::string(25, '[') + "0" + std::string(25, ']')); badJson(s);
    s = json(wire()); pos = s.find("\"light\""); s.replace(pos, 7, std::string(25, '[') + "0"); badJson(s);
    s = json(wire()); badJson(s.substr(0, s.size() - 1));
}
void revision_decimal_roundtrip() {
    begin("revision_decimal_roundtrip");
    for (auto revision : {UINT64_C(9223372036854775807), UINT64_C(9223372036854775808), UINT64_MAX}) {
        auto d = good(); d.revision = revision; auto v = wire(d); CHECK(v["revision"].isString()); CHECK(v["revision"].toString().toStdString() == std::to_string(revision));
        RackDesign out; CHECK(decodeRackDesignObject(v, out, RackValidationMode::portableDesign)); CHECK(out == d);
        juce::MemoryBlock b; CHECK(encodeRackDesign(d, b)); CHECK(decodeRackDesign(b.getData(), b.getSize(), out)); CHECK(out == d);
    }
    for (const auto* text : {"18446744073709551616", "0", "01", "+1", "-1", " 1", "1 ", "1.0", "1e1", "", "000", "184467440737095516150"}) { auto v = wire(); v.getDynamicObject()->setProperty("revision", text); badObject(v); }
    for (auto value : {juce::var(1), juce::var(1.), juce::var(true)}) { auto v = wire(); v.getDynamicObject()->setProperty("revision", value); badObject(v); }
    auto schema = rackDesignSchema(); auto r = schema["properties"]["revision"]; CHECK(r["type"].toString() == "string"); CHECK(int(r["maxLength"]) == 20); CHECK(r["pattern"].toString() == "^[1-9][0-9]{0,19}$"); CHECK(!bool(schema["additionalProperties"]));
    auto anonymous = rackDesignSchema(false); CHECK(!anonymous["properties"].getDynamicObject()->hasProperty("id")); CHECK(!anonymous["properties"].getDynamicObject()->hasProperty("revision"));
}
void portable_semantics() {
    begin("portable_semantics");
    CHECK(rackParameterIndex(NativeModule::analog, 0, "analog.filter.cutoff") == std::optional<std::size_t>(39));
    CHECK(rackParameterIndex(NativeModule::analog, 3, "analog.filter.cutoff") == std::optional<std::size_t>(108));
    CHECK(rackParameterIndex(NativeModule::drive, 0, "drive.gain_db") == std::optional<std::size_t>(573));
    CHECK(rackParameterIndex(NativeModule::drive, 7, "drive.gain_db") == std::optional<std::size_t>(734));
    CHECK(!rackParameterIndex(NativeModule::analog, 4, "level")); CHECK(!rackParameterIndex(NativeModule::drive, 8, "bypassed"));
    auto d = good(); d.units[0].module = NativeModule::wavetable; CHECK(validateRackDesign(d, RackValidationMode::instanceRecall)); bad(d);
    juce::MemoryBlock b; CHECK(encodeRackDesign(d, b, RackValidationMode::instanceRecall)); RackDesign out; CHECK(decodeRackDesign(b.getData(), b.getSize(), out, RackValidationMode::instanceRecall)); CHECK(out == d); CHECK(!decodeRackDesign(b.getData(), b.getSize(), out)); CHECK(out == d);
    d.panel.controls[0].unit = 2; CHECK(!validateRackDesign(d, RackValidationMode::instanceRecall)); d = good(); d.panel.controls[0].key = "deleted"; CHECK(!validateRackDesign(d, RackValidationMode::instanceRecall));
    d = good(); d.role = RackRole::fx; d.units = {{0, NativeModule::delay, {{0, "drive.gain_db", .5f}}}}; d.panel.controls = {{"Gain",0,0,"drive.gain_db",FocusWidget::rotary,FocusResponse::linear,0,24}}; CHECK(validateRackDesign(d, RackValidationMode::instanceRecall)); bad(d);
    auto text = json(wire()); CHECK(text.find("synth.") == std::string::npos); CHECK(text.find("slot") == std::string::npos); CHECK(text.find("bindings") == std::string::npos);
    d = good(); d.intent = "Literal \\u0000 text"; CHECK(encodeRackDesign(d, b)); CHECK(decodeRackDesign(b.getData(), b.getSize(), out)); CHECK(out == d);
    d = good(); d.units[0].defaults = {{0, "enabled", 0}, {0, "mute", 1}, {0, "analog.filter.cutoff", .5f}}; CHECK(validateRackDesign(d, RackValidationMode::portableDesign));
    d.role = RackRole::fx; d.units = {{0, NativeModule::drive, {{0, "bypassed", 1}}}}; d.panel.controls = {{"Gain",0,0,"drive.gain_db",FocusWidget::rotary,FocusResponse::linear,0,24}}; CHECK(validateRackDesign(d, RackValidationMode::portableDesign));
    // Exercise the complete legal retained recipe, with maximum text/revision,
    // rather than treating each bound as an isolated scalar check.
    d = good(); d.name = juce::String::repeatedString(juce::String::fromUTF8("\xc3\xa9"), 32);
    d.intent = juce::String::repeatedString(d.name, 8); d.revision = UINT64_MAX;
    d.panel.sections.resize(4, d.name); d.units[0].defaults.clear();
    const auto registry = parameterRegistry();
    for (const auto& p : registry.first(drumParameterStart)) if (p.id.startsWith("synth.1.")) d.units[0].defaults.push_back({0, p.id.substring(8), 0});
    CHECK(d.units[0].defaults.size() == 149);
    for (int i = 1; i <= 8; ++i) {
        RackUnit u {static_cast<std::uint8_t>(i), static_cast<NativeModule>(2 + (i - 1) % 4), {}};
        for (const auto& p : registry.first(dynamicEqParameterStart)) if (p.id.startsWith("effect.1.")) u.defaults.push_back({u.id, p.id.substring(9), 0});
        CHECK(u.defaults.size() == 82); d.units.push_back(std::move(u));
    }
    d.panel.controls.clear();
    for (int i = 0; i < 8; ++i) d.panel.controls.push_back({d.name, static_cast<std::uint8_t>(i + 1), static_cast<std::uint8_t>(i % 4), "bypassed", FocusWidget::toggle, FocusResponse::linear, 0, 1});
    CHECK(validateRackDesign(d, RackValidationMode::instanceRecall));
    CHECK(encodeRackDesign(d, b, RackValidationMode::instanceRecall)); CHECK(b.getSize() <= 65536);
    CHECK(decodeRackDesign(b.getData(), b.getSize(), out, RackValidationMode::instanceRecall)); CHECK(out == d);
    CHECK(!validateRackDesign(d, RackValidationMode::portableDesign));
    auto excessive = d; excessive.units[0].defaults.push_back(excessive.units[0].defaults[0]); CHECK(!validateRackDesign(excessive, RackValidationMode::instanceRecall));
    excessive = d; excessive.units[1].defaults.push_back(excessive.units[1].defaults[0]); CHECK(!validateRackDesign(excessive, RackValidationMode::instanceRecall));
    for (auto& u : d.units) std::erase_if(u.defaults, [&](const auto& p) {
        if (u.module == NativeModule::analog) return p.key.startsWith("wavetable.");
        return p.key != "bypassed" && !p.key.startsWith(juce::String(u.module == NativeModule::drive ? "drive." : u.module == NativeModule::delay ? "delay." : u.module == NativeModule::reverb ? "reverb." : "compressor."));
    });
    CHECK(d.units[0].defaults.size() == 117); CHECK(validateRackDesign(d, RackValidationMode::portableDesign)); CHECK(encodeRackDesign(d, b)); CHECK(decodeRackDesign(b.getData(), b.getSize(), out)); CHECK(out == d);
}
void stable_registry() {
    begin("stable_registry"); const auto registry = parameterRegistry(); CHECK(registry.size() == parameterCount);
    // FNV64 snapshot of every id/order/stateKey/default bit; registry source
    // blobs are separately verified identical to the original HEAD.
    std::uint64_t hash = UINT64_C(14695981039346656037);
    auto byte = [&](std::uint8_t b) { hash = (hash ^ b) * UINT64_C(1099511628211); };
    for (const auto& p : registry.first(758)) {
        for (auto c : p.id.toStdString()) byte(static_cast<std::uint8_t>(c)); byte(0);
        for (auto n : {p.stateKey, std::bit_cast<std::uint32_t>(p.defaultValue)}) for (int i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(n >> (i * 8)));
        if (p.id.startsWith("synth.") || p.id.startsWith("effect.")) {
            auto suffix = p.id.fromFirstOccurrenceOf(".", false, false).fromFirstOccurrenceOf(".", false, false);
            CHECK(suffix.getNumBytesAsUTF8() <= 32); for (auto c : suffix.toStdString()) CHECK(static_cast<unsigned char>(c) < 128);
        }
    }
    std::cout << "registry baseline " << hash << '\n';
    CHECK(hash == UINT64_C(7310457928473826962));
}
void legal_physical_endpoint_roundtrip() {
    begin("legal_physical_endpoint_roundtrip");
    auto roundtrip = [](RackDesign d) {
        const auto& control = d.panel.controls[0];
        CHECK(validateRackDesign(d, RackValidationMode::portableDesign));
        juce::var object;
        CHECK(encodeRackDesignObject(d, object, RackValidationMode::portableDesign));
        RackDesign objectResult;
        CHECK(decodeRackDesignObject(object, objectResult, RackValidationMode::portableDesign));
        CHECK(objectResult == d);
        juce::MemoryBlock bytes;
        CHECK(encodeRackDesign(d, bytes));
        CHECK(bytes.getSize() <= 65536);
        RackDesign fileResult;
        CHECK(decodeRackDesign(bytes.getData(), bytes.getSize(), fileResult));
        CHECK(fileResult == d);
        juce::var parsed;
        const bool parsedOk = ai::parseStrictJson({static_cast<const char*>(bytes.getData()), bytes.getSize()}, parsed, 65536);
        CHECK(parsedOk);
        if (!parsedOk) return;
        const auto* controls = parsed["panel"]["controls"].getArray();
        CHECK(controls != nullptr && controls->size() == 1);
        if (controls == nullptr || controls->size() != 1) return;
        // Float endpoints are widened exactly into JUCE var doubles. Text must
        // preserve those doubles, since admission compares before narrowing.
        CHECK(double(parsed["panel"]["controls"][0]["minimum"]) == double(control.minimum));
        CHECK(double(parsed["panel"]["controls"][0]["maximum"]) == double(control.maximum));
        RackDesign parsedResult;
        CHECK(decodeRackDesignObject(parsed, parsedResult, RackValidationMode::portableDesign));
        CHECK(parsedResult == d);
    };
    auto pulse = good();
    pulse.units[0].defaults = {{0, "analog.osc.1.pulse_width", std::numeric_limits<float>::denorm_min()}};
    pulse.panel.controls = {{"Width", 0, 0, "analog.osc.1.pulse_width", FocusWidget::rotary, FocusResponse::linear, .05f, .95f}};
    const auto beforePulse = failures;
    roundtrip(pulse);
    std::cout << "pulse_width reproduction failures=" << failures - beforePulse << '\n';

    int endpoints = 0;
    for (const auto& descriptor : parameterRegistry()) {
        const bool isSynth = descriptor.id.startsWith("synth.");
        if (descriptor.kind != ParameterKind::continuous || (!isSynth && !descriptor.id.startsWith("effect."))) continue;
        auto d = good();
        const auto key = descriptor.id.substring(isSynth ? 8 : 9);
        NativeModule module = NativeModule::analog;
        if (isSynth) {
            if (key.startsWith("wavetable.")) module = NativeModule::wavetable;
        } else {
            d.role = RackRole::fx;
            module = key.startsWith("drive.") ? NativeModule::drive : key.startsWith("delay.") ? NativeModule::delay
                : key.startsWith("reverb.") ? NativeModule::reverb : key.startsWith("eq.") ? NativeModule::eq : key.startsWith("filter_fx.") ? NativeModule::filter : key.startsWith("chorus.") ? NativeModule::chorus : key.startsWith("phaser.") ? NativeModule::phaser : key.startsWith("flanger.") ? NativeModule::flanger : key.startsWith("stereo.") ? NativeModule::stereo : key.startsWith("dimension.") ? NativeModule::dimension : key.startsWith("frequency_shift.") ? NativeModule::frequencyShift : key.startsWith("convolution.") ? NativeModule::convolution : key.startsWith("frequency_split.") ? NativeModule::frequencySplit : key.startsWith("mid_side_split.") ? NativeModule::midSideSplit : key.startsWith("dynamic_eq.") ? NativeModule::dynamicEq : key.startsWith("space_reverb.") ? NativeModule::spaceReverb : key.startsWith("era_reverb.") ? NativeModule::eraReverb : NativeModule::compressor;
        }
        if (!rackParameterActive(module, key)) continue;
        d.units = {{0, module, {{0, key, .1234567f}}}};
        d.panel.controls = {{"Endpoint", 0, 0, key, FocusWidget::verticalSlider, FocusResponse::linear, descriptor.minimum, descriptor.maximum}};
        const auto before = failures;
        roundtrip(d);
        if (before != failures) std::cout << "endpoint failures: " << descriptor.id << '\n';
        ++endpoints;
    }
    std::cout << "active continuous registry endpoint designs=" << endpoints << '\n';
    CHECK(endpoints > 0);
    // The public strict parser must retain the scanner's precise conversion,
    // even when JUCE's underlying reader would truncate fractional digits.
    auto delay = good();
    delay.role = RackRole::fx;
    delay.units = {{0, NativeModule::delay, {}}};
    delay.panel.controls = {{"Time", 0, 0, "delay.seconds", FocusWidget::rotary, FocusResponse::linear, .001f, 4}};
    juce::MemoryBlock delayBytes;
    CHECK(encodeRackDesign(delay, delayBytes));
    const std::string delayJson(static_cast<const char*>(delayBytes.getData()), delayBytes.getSize());
    const auto minimumStart = delayJson.find("\"minimum\":") + std::string("\"minimum\":").size();
    const auto minimumEnd = delayJson.find(',', minimumStart);
    const auto minimumToken = juce::String(delayJson.substr(minimumStart, minimumEnd - minimumStart)).trim().toStdString();
    double accurate {};
    const auto converted = std::from_chars(minimumToken.data(), minimumToken.data() + minimumToken.size(), accurate);
    CHECK(converted.ec == std::errc{} && converted.ptr == minimumToken.data() + minimumToken.size());
    CHECK(accurate == double(.001f));
    juce::var parsedDelay;
    CHECK(ai::parseStrictJson(delayJson, parsedDelay, 65536));
    const auto parsedMinimum = double(parsedDelay["panel"]["controls"][0]["minimum"]);
    std::cout << "delay minimum token=" << minimumToken
        << " exact=" << juce::String(double(.001f), 22)
        << " strict parsed=" << juce::String(parsedMinimum, 22) << '\n';
    CHECK(parsedMinimum == accurate);
    const auto scientific = juce::String(double(.001f), std::numeric_limits<double>::max_digits10, true);
    juce::var scientificObject;
    CHECK(ai::parseStrictJson("{\"value\":" + scientific.toStdString() + "}", scientificObject));
    CHECK(double(scientificObject["value"]) == double(.001f));
    std::cout << "scientific token=" << scientific << " exact strict parse="
        << (double(scientificObject["value"]) == double(.001f)) << '\n';
}
}
void active_parameter_policy() {
    begin("active_parameter_policy");
    for (unsigned m = 0; m < 17; ++m) {
        const auto module = static_cast<NativeModule>(m);
        const bool isSynth = m < 2;
        auto d = good();
        d.role = isSynth ? RackRole::synth : RackRole::fx;
        d.units = {{0, module, {}}};
        d.panel.controls.clear();
        const juce::String prefix = isSynth ? "synth.1." : "effect.1.";
        for (const auto& p : parameterRegistry()) {
            if (!p.id.startsWith(prefix)) continue;
            const auto key = p.id.substring(prefix.length());
            const bool expected = isSynth
                ? key != "drum_mode" && !key.startsWith("drum.") && (!key.startsWith("analog.") || m == 0) && (!key.startsWith("wavetable.") || m == 1)
                : key == "bypassed" || key.startsWith(juce::String(m == 2 ? "drive." : m == 3 ? "delay." : m == 4 ? "reverb." : m == 5 ? "compressor." : m == 6 ? "eq." : m == 7 ? "filter_fx." : m == 8 ? "chorus." : m == 9 ? "phaser." : m == 10 ? "flanger." : m == 11 ? "stereo." : m == 12 ? "dimension." : m == 13 ? "frequency_shift." : m == 14 ? "convolution." : m == 15 ? "frequency_split." : "mid_side_split."));
            CHECK(rackParameterActive(module, key) == expected);
            d.units[0].defaults = {{0, key, key == "engine" && m == 1 ? 1.f : 0.f}};
            CHECK(validateRackDesign(d, RackValidationMode::instanceRecall) == (key != "drum_mode"));
            CHECK(validateRackDesign(d, RackValidationMode::portableDesign) == expected);
        }
    }
}
void algorithm_revision_transaction() {
    begin("algorithm_revision_transaction: portable and legacy unit revisions");
    const auto original=good();const auto encoded=wire();CHECK(int(encoded["version"])==2);
    for(unsigned m=0;m<17;++m){CHECK(supportsAlgorithm(static_cast<NativeModule>(m),1));CHECK(!supportsAlgorithm(static_cast<NativeModule>(m),0));CHECK(supportsAlgorithm(static_cast<NativeModule>(m),2)==(m<=8||m==10||m==12||m==15));CHECK(!supportsAlgorithm(static_cast<NativeModule>(m),3));}
    auto unsupported=original;unsupported.units[0].algorithmRevision=3;bad(unsupported);
    auto invalid=wire();invalid["units"][0].getDynamicObject()->setProperty("algorithm_revision",3);badObject(invalid);
    for(auto module:{NativeModule::analog,NativeModule::wavetable}){
        auto modern=original;modern.units[0].module=module;modern.units[0].algorithmRevision=2;
        modern.units[0].defaults={{0,module==NativeModule::analog?"analog.filter.cutoff":"wavetable.filter.cutoff",.5f}};
        modern.panel.controls[0].key=modern.units[0].defaults[0].key;
        CHECK(validateRackDesign(modern,RackValidationMode::portableDesign));
        juce::MemoryBlock bytes;CHECK(encodeRackDesign(modern,bytes));RackDesign out;
        CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),out)&&out==modern);
    }
    invalid=wire();invalid["units"][0].getDynamicObject()->removeProperty("algorithm_revision");badObject(invalid);
    auto legacy=wire();legacy.getDynamicObject()->setProperty("version",1);
    for(auto& unit:*legacy["units"].getArray())unit.getDynamicObject()->removeProperty("algorithm_revision");
    RackDesign recalled;CHECK(decodeRackDesignObject(legacy,recalled,RackValidationMode::portableDesign));CHECK(recalled==original);
    juce::var modern;CHECK(encodeRackDesignObject(recalled,modern,RackValidationMode::portableDesign));CHECK(int(modern["version"])==2);
    CHECK(recalled.revision==original.revision);for(const auto& unit:recalled.units)CHECK(unit.algorithmRevision==1);
}
// Copper Arc and Prism Relay moved to the revision-2 voice under new design IDs.
// Their revision-1 definitions live on only inside projects that embedded them.
struct SupersededFactorySynth { const char* revised; const char* revisedSha256; const char* superseded; const char* supersededSha256; float supersededLevel; };
constexpr SupersededFactorySynth supersededFactorySynths[]={
    {"91387715-13d3-4c57-a479-12251842a402","f034d6ab6c84c3daaf1d176034d3097e226e3aeb08f3907f68aad1870635206e","91387715-13d3-4c57-a479-12251842a401","3a3029c1af239e0d0b38bcc335445017b0099f322aec93ce237f27ddffad5fa6",.7f},
    {"91387715-13d3-4c57-a479-12251842b402","e25107fe9206584ebcbb2b3ee8021710270084fdbd2b843cacbb30c377d99dc9","91387715-13d3-4c57-a479-12251842b401","9b3b96fcbf17afb123d118d550149d160beafccc1f50134504710ec95d52191e",.7f}};
std::optional<RackDesign> factoryDesign(const char* id) {
    const auto factories=airack::test::frozenFactoryRackDesigns();
    const auto found=std::find_if(factories.begin(),factories.end(),[&](const auto& d){return d.id==juce::Uuid(id);});
    if(found==factories.end())return std::nullopt;
    return *found;
}
std::optional<RackDesign> revisedFactorySynth(bool digital) { return factoryDesign(supersededFactorySynths[digital].revised); }
RackDesign supersededFactoryDesign(RackDesign design,bool digital) {
    const auto& entry=supersededFactorySynths[digital];auto& unit=design.units[0];
    design.id=juce::Uuid(entry.superseded);unit.algorithmRevision=legacyAlgorithmRevision;
    const auto& level=parameterRegistry()[*rackParameterIndex(unit.module,0,"level")];
    for(auto& value:unit.defaults)if(value.key=="level")value.normalized=juce::NormalisableRange<float>(level.minimum,level.maximum,0,level.skew).convertTo0to1(entry.supersededLevel);
    return design;
}
// Cinder Coil moved to drive revision 2 the same way, keeping its settings.
constexpr const char* revisedCinderCoil="91387715-13d3-4c57-a479-12251842e402";
constexpr const char* revisedCinderCoilSha256="74a971b94d9ea607f46b914f70790143c20e6b9d691709068a4a50e4108496a6";
constexpr const char* supersededCinderCoil="91387715-13d3-4c57-a479-12251842e401";
constexpr const char* supersededCinderCoilSha256="f91d423310f5ec2ba269cbbfec4db640808acc9b5ac86d7bbb852578365abadd";
RackDesign supersededFactoryDrive(RackDesign design) {
    design.id=juce::Uuid(supersededCinderCoil);design.units[0].algorithmRevision=legacyAlgorithmRevision;
    return design;
}
// Anchor Press moved to compressor revision 2 the same way; only its threshold may move.
constexpr const char* revisedAnchorPress="91387715-13d3-4c57-a479-12251842d402";
constexpr const char* revisedAnchorPressSha256="28aa8a7fe14bc12636cf5a49785ee7cdb2eeea1bdb374239d7fdb311359d37ce";
constexpr const char* supersededAnchorPress="91387715-13d3-4c57-a479-12251842d401";
constexpr const char* supersededAnchorPressSha256="9ac00f9ae77a01e30b16c70d9f6426730f5417d148712a1d05e24adea9be2294";
constexpr float supersededAnchorPressThresholdDb=-54,revisedAnchorPressThresholdDb=-50.25f;
RackDesign supersededFactoryCompressor(RackDesign design) {
    auto& unit=design.units[0];design.id=juce::Uuid(supersededAnchorPress);unit.algorithmRevision=legacyAlgorithmRevision;
    const auto& threshold=parameterRegistry()[*rackParameterIndex(unit.module,0,"compressor.threshold_db")];
    for(auto& value:unit.defaults)if(value.key=="compressor.threshold_db")
        value.normalized=juce::NormalisableRange<float>(threshold.minimum,threshold.maximum,0,threshold.skew).convertTo0to1(supersededAnchorPressThresholdDb);
    return design;
}
// Orbit Ribbon, Silver Loop and Twin Current moved to the revision-2 modulated taps, Echo Thread
// to delay revision 2, Contour Loom to EQ revision 2, Veil Current to filter revision 2 and Band
// Balance to frequency-split revision 2, the same way, keeping their settings.
struct SupersededFactoryTap {
    EffectKind kind; std::size_t bank; const char* name; const char* revised; const char* revisedSha256;
    const char* superseded; const char* supersededSha256; const char* savedKey; const char* instance; const char* recallSection; const char* sustainSection;
};
constexpr SupersededFactoryTap supersededFactoryTaps[]={
    {EffectKind::chorus,chorusEffectBank,"Orbit Ribbon","91387715-13d3-4c57-a479-12251842c802","4e2e5c7b49446deca5cd23952614c3e147bf7d09ea045409878a191e68697a1c",
     "91387715-13d3-4c57-a479-12251842c801","ca33524b056b504e69c4db169a8e7b8accd9a553e00dfb8760805bed25963892","chorus.mix","91387715-13d3-4c57-a479-12251842c813","superseded_factory_chorus_recall_and_catalog","factory_chorus_sustain_matches_superseded"},
    {EffectKind::flanger,flangerEffectBank,"Silver Loop","91387715-13d3-4c57-a479-12251842e902","bebd5a6b4e00728cade0e148ab2acd3dc2e7ee59b604868c5fca495734c168a5",
     "91387715-13d3-4c57-a479-12251842e901","0a23b2a9304bd5a516e88ab39145b6a64a313f132ee5ab2508822b4303e6d35b","flanger.mix","91387715-13d3-4c57-a479-12251842e913","superseded_factory_flanger_recall_and_catalog","factory_flanger_sustain_matches_superseded"},
    {EffectKind::dimension,dimensionEffectBank,"Twin Current","91387715-13d3-4c57-a479-12251842aa02","3a142158a0d83bd0fdf392ac2f9d1a71a2ca9780c29499f0c1375cddf68e7d9d",
     "91387715-13d3-4c57-a479-12251842aa01","efa26b4a68a504daf7a5eef3086480ad5cbde1b5ef60c80505f7ae23babf968f","dimension.mix","91387715-13d3-4c57-a479-12251842aa13","superseded_factory_dimension_recall_and_catalog","factory_dimension_sustain_matches_superseded"},
    {EffectKind::delay,delayEffectBank,"Echo Thread","91387715-13d3-4c57-a479-12251842f402","c0517e63367b7f2d7331b4037b069f9c2bc37cf222d59f63ec0acf6a0454f2e5",
     "91387715-13d3-4c57-a479-12251842f401","189355f5b130c06ec606d5ed219d57e38b8d5dddb961043cb02ff1042017933b","delay.mix","91387715-13d3-4c57-a479-12251842f413","superseded_factory_delay_recall_and_catalog","factory_delay_sustain_matches_superseded"},
    {EffectKind::eq,eqEffectBank,"Contour Loom","91387715-13d3-4c57-a479-12251842a802","ddf918a3b7c368b8355b871ab03973af1243a7b59d380ed80ff8e988fe6d00e4",
     "91387715-13d3-4c57-a479-12251842a801","141cb83a5e1320023f97e322befcd8a2bd5de0da86ee2081ad40862b84c11090","eq.bell.gain_db","91387715-13d3-4c57-a479-12251842a813","superseded_factory_eq_recall_and_catalog","factory_eq_sustain_matches_superseded"},
    {EffectKind::filter,filterEffectBank,"Veil Current","91387715-13d3-4c57-a479-12251842b802","a4b40720a95ba809a2da812324bfe2a2b7ce731f1d40723fdcd57860d14f6c2b",
     "91387715-13d3-4c57-a479-12251842b801","b5e4d606d0b2817d42561ac4d3c83cf77213f4289d64ae02780566b032d79d2b","filter_fx.cutoff_hz","91387715-13d3-4c57-a479-12251842b813","superseded_factory_filter_recall_and_catalog","factory_filter_sustain_matches_superseded"},
    {EffectKind::frequencySplit,frequencySplitEffectBank,"Band Balance","91387715-13d3-4c57-a479-12251842bd02","9640d0638bf1703204b46443b3a0818765e4b4fe4cf5485861a81dd7c13f1bab",
     "91387715-13d3-4c57-a479-12251842bd01","f82081e30e32b3947dd0e0da7983def088d888ac5fdf188a489c8cecda5e0196","frequency_split.crossover_hz","91387715-13d3-4c57-a479-12251842bd13","superseded_factory_frequency_split_recall_and_catalog","factory_frequency_split_sustain_matches_superseded"}};
RackDesign supersededFactoryTap(RackDesign design,const SupersededFactoryTap& entry) {
    design.id=juce::Uuid(entry.superseded);design.units[0].algorithmRevision=legacyAlgorithmRevision;
    return design;
}
void setPhysical(SoundDocument& document,const char* id,float physical) {
    const auto registry=parameterRegistry();
    const auto found=std::find_if(registry.begin(),registry.end(),[&](const auto& p){return p.id==id;});
    CHECK(found!=registry.end());if(found==registry.end())return;
    document.values[std::size_t(found-registry.begin())]=juce::NormalisableRange<float>(found->minimum,found->maximum,0,found->skew).convertTo0to1(physical);
}
void factory_placement_and_recall(bool digital) {
    begin(digital?"factory_digital_placement_and_recall":"factory_analog_placement_and_recall");
    // Preserve the pre-stereo-mode Pro-R example authority alongside main's revised recipe.
    juce::MemoryOutputStream historicalOxide;
    CHECK(juce::Base64::convertFromBase64(historicalOxide,
        "eyJ2ZXJzaW9uIjogMiwgImlkIjogIjMzMGNiNTNiLWNkOWItNDQzYy05NGQ4LTI3MDBiYzhiODhmNyIsICJyZXZpc2lvbiI6ICIxIiwgIm5hbWUiOiAiT3hp"
        "ZGUgUmV0dXJuIiwgImludGVudCI6ICJBIHdhcm0gdGFwZS1zdHlsZSBlY2hvIHdob3NlIHJlcGVhdHMgc2F0dXJhdGUsIGRhcmtlbiBhbmQgdGhpbiBhcyB0"
        "aGV5IHJlY2lyY3VsYXRlLCB3aXRoIGEgZ2VudGxlIHdvdzsgdGltZSBjaGFuZ2VzIGdsaWRlIGxpa2UgYSB0YXBlIHRyYW5zcG9ydC4gU2hhcGUgdGhlIGVj"
        "aG8gdGltZSwgaG93IGxvbmcgdGhlIHJlcGVhdHMgcmV0dXJuLCBob3cgbXVjaCB0YXBlIGdyaXQgYW5kIHdvYmJsZSB0aGV5IGdhdGhlciBhbmQgdGhlIGxv"
        "dyBhbmQgaGlnaCBjdXQgb24gdGhlIHJlcGVhdHM7IHRoZSBkcnkgc2lnbmFsIHN0YXlzIGNsZWFuLiBTd2l0Y2ggdG8gaG9zdC1zeW5jZWQgbm90ZSB2YWx1"
        "ZXMsIGNyb3NzIHRoZSByZXBlYXRzIGJldHdlZW4gdGhlIHNpZGVzLCBvciBzcGVlZCB0aGUgd29iYmxlIHVwIHRvd2FyZCBmbHV0dGVyIHdoZW4gbmVlZGVk"
        "LiIsICJyb2xlIjogImZ4IiwgInVuaXRzIjogW3siaWQiOiAwLCAibW9kdWxlIjogImRlbGF5IiwgImRlZmF1bHRzIjogW3sidW5pdCI6IDAsICJrZXkiOiAi"
        "YnlwYXNzZWQiLCAibm9ybWFsaXplZCI6IDAuMH0sIHsidW5pdCI6IDAsICJrZXkiOiAiZGVsYXkuc3luY2VkIiwgIm5vcm1hbGl6ZWQiOiAwLjB9LCB7InVu"
        "aXQiOiAwLCAia2V5IjogImRlbGF5LnNlY29uZHMiLCAibm9ybWFsaXplZCI6IDAuMDc0NzY4Njk5NzA1NjAwNzM4NTI1NH0sIHsidW5pdCI6IDAsICJrZXki"
        "OiAiZGVsYXkuYmVhdHMiLCAibm9ybWFsaXplZCI6IDAuNDY2NjY2NjY4NjUzNDg4MTU5MTc5N30sIHsidW5pdCI6IDAsICJrZXkiOiAiZGVsYXkuZmVlZGJh"
        "Y2siLCAibm9ybWFsaXplZCI6IDAuNDAwMDAwMDA1OTYwNDY0NDc3NTM5MX0sIHsidW5pdCI6IDAsICJrZXkiOiAiZGVsYXkuc3RlcmVvIiwgIm5vcm1hbGl6"
        "ZWQiOiAwLjMwMDAwMDAxMTkyMDkyODk1NTA3ODF9LCB7InVuaXQiOiAwLCAia2V5IjogImRlbGF5LmZpbHRlcl9oeiIsICJub3JtYWxpemVkIjogMC4yNDky"
        "NDkyNDk2OTY3MzE1NjczODI4fSwgeyJ1bml0IjogMCwgImtleSI6ICJkZWxheS5taXgiLCAibm9ybWFsaXplZCI6IDAuMjV9LCB7InVuaXQiOiAwLCAia2V5"
        "IjogImRlbGF5LnNhdHVyYXRpb24iLCAibm9ybWFsaXplZCI6IDAuMzAwMDAwMDExOTIwOTI4OTU1MDc4MX0sIHsidW5pdCI6IDAsICJrZXkiOiAiZGVsYXku"
        "bG93X2N1dF9oeiIsICJub3JtYWxpemVkIjogMC4wNDA0MDQwNDAzNjY0MTEyMDkxMDY0fSwgeyJ1bml0IjogMCwgImtleSI6ICJkZWxheS53b2JibGUiLCAi"
        "bm9ybWFsaXplZCI6IDAuMjAwMDAwMDAyOTgwMjMyMjM4NzY5NX0sIHsidW5pdCI6IDAsICJrZXkiOiAiZGVsYXkud29iYmxlX3JhdGVfaHoiLCAibm9ybWFs"
        "aXplZCI6IDAuMDcwNzA3MDc1Mjk3ODMyNDg5MDEzN30sIHsidW5pdCI6IDAsICJrZXkiOiAiZGVsYXkuZ2xpZGUiLCAibm9ybWFsaXplZCI6IDEuMH1dLCAi"
        "YWxnb3JpdGhtX3JldmlzaW9uIjogMn1dLCAicGFuZWwiOiB7ImxheW91dCI6ICJvcmRlcmVkRnhTdHJpcCIsICJzZWN0aW9ucyI6IFsiRWNobyIsICJUYXBl"
        "IiwgIlRvbmUiLCAiT3V0cHV0Il0sICJjb250cm9scyI6IFt7ImxhYmVsIjogIlRpbWUiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMCwgImtleSI6ICJkZWxh"
        "eS5zZWNvbmRzIiwgIndpZGdldCI6ICJyb3RhcnkiLCAicmVzcG9uc2UiOiAibG9nYXJpdGhtaWMiLCAibWluaW11bSI6IDAuMDE5OTk5OTk5NTUyOTY1MTY0"
        "MTg0NiwgIm1heGltdW0iOiAyLjB9LCB7ImxhYmVsIjogIlJlcGVhdHMiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMCwgImtleSI6ICJkZWxheS5mZWVkYmFj"
        "ayIsICJ3aWRnZXQiOiAicm90YXJ5IiwgInJlc3BvbnNlIjogImxpbmVhciIsICJtaW5pbXVtIjogMC4wLCAibWF4aW11bSI6IDAuOTQ5OTk5OTg4MDc5MDcx"
        "MDQ0OTIxOX0sIHsibGFiZWwiOiAiU2F0dXJhdGlvbiIsICJ1bml0IjogMCwgInNlY3Rpb24iOiAxLCAia2V5IjogImRlbGF5LnNhdHVyYXRpb24iLCAid2lk"
        "Z2V0IjogInJvdGFyeSIsICJyZXNwb25zZSI6ICJsaW5lYXIiLCAibWluaW11bSI6IDAuMCwgIm1heGltdW0iOiAxLjB9LCB7ImxhYmVsIjogIldvYmJsZSIs"
        "ICJ1bml0IjogMCwgInNlY3Rpb24iOiAxLCAia2V5IjogImRlbGF5LndvYmJsZSIsICJ3aWRnZXQiOiAicm90YXJ5IiwgInJlc3BvbnNlIjogImxpbmVhciIs"
        "ICJtaW5pbXVtIjogMC4wLCAibWF4aW11bSI6IDEuMH0sIHsibGFiZWwiOiAiTG93IGN1dCIsICJ1bml0IjogMCwgInNlY3Rpb24iOiAyLCAia2V5IjogImRl"
        "bGF5Lmxvd19jdXRfaHoiLCAid2lkZ2V0IjogInJvdGFyeSIsICJyZXNwb25zZSI6ICJsb2dhcml0aG1pYyIsICJtaW5pbXVtIjogMjAuMCwgIm1heGltdW0i"
        "OiAyMDAwLjB9LCB7ImxhYmVsIjogIkhpZ2ggY3V0IiwgInVuaXQiOiAwLCAic2VjdGlvbiI6IDIsICJrZXkiOiAiZGVsYXkuZmlsdGVyX2h6IiwgIndpZGdl"
        "dCI6ICJyb3RhcnkiLCAicmVzcG9uc2UiOiAibG9nYXJpdGhtaWMiLCAibWluaW11bSI6IDEwMDAuMCwgIm1heGltdW0iOiAyMDAwMC4wfSwgeyJsYWJlbCI6"
        "ICJNaXgiLCAidW5pdCI6IDAsICJzZWN0aW9uIjogMywgImtleSI6ICJkZWxheS5taXgiLCAid2lkZ2V0IjogInJvdGFyeSIsICJyZXNwb25zZSI6ICJsaW5l"
        "YXIiLCAibWluaW11bSI6IDAuMCwgIm1heGltdW0iOiAxLjB9XSwgInN1cmZhY2UiOiAibGlnaHQiLCAiYWNjZW50IjogIm5ldXRyYWwiLCAiZnJhbWUiOiAi"
        "cm91bmRlZCIsICJlbXBoYXNpcyI6ICJiYWxhbmNlZCIsICJzcGFjaW5nIjogImNvbXBhY3QiLCAiZGVuc2l0eSI6ICJzcGFyc2UifX0="));
    const auto oldOxideBytes=historicalOxide.getMemoryBlock();
    CHECK(juce::SHA256(oldOxideBytes).toHexString()=="544009d613a14de73f952127f2e85dd5dcaeb67120c9b5ff8a808f72437d0699");
    RackDesign oldOxide;CHECK(decodeRackDesign(oldOxideBytes.getData(),oldOxideBytes.getSize(),oldOxide));
    juce::MemoryBlock oldOxideRoundtrip;CHECK(encodeRackDesign(oldOxide,oldOxideRoundtrip)&&oldOxideRoundtrip==oldOxideBytes);
    const auto historical = airack::test::frozenExampleRackDesigns();
    constexpr const char* historicalHashes[]={"1b61f57c3fb42da21cfc53c39bd309ce58cb6017415f556add7d72b42cf4911a","8b1afcfbcef817e372928a9d8ff80ef53b8731043b77096968e2dd966dcfe8f4","cff62f76f8baec2a7f8d153a953f55b1ce69ba6fab0432d99577ddaeac6569ed","61fb7252e7e804e9742fee81ee7befa5f5778e3bac43c344dca85609037952c0","82154ffc00dbe85e4af0d2041da99b1f52a3b9888bea29f16b4bd2d2bfe61c69","55eb1f05ca9ba938034ede012941c197498a87e89c0af5fa52ef29b936064054"};
    for (std::size_t i=0;i<historical.size();++i) {
        juce::MemoryBlock bytes; CHECK(encodeRackDesign(historical[i],bytes));
        std::cout << "LEGACY_EXAMPLE " << i << ' ' << historical[i].id.toString() << ' ' << juce::SHA256(bytes).toHexString() << '\n';
        CHECK(juce::SHA256(bytes).toHexString()==historicalHashes[i]);
    }
    const auto factories = airack::test::frozenFactoryRackDesigns();
    CHECK(!factories.empty()); if (factories.empty()) return;
    const auto module=digital?NativeModule::wavetable:NativeModule::analog;
    const juce::String instancePrefix=digital?"91387715-13d3-4c57-a479-12251842b41":"91387715-13d3-4c57-a479-12251842a41";
    const auto found = std::find_if(factories.begin(),factories.end(),[&](const auto& d){return d.id==juce::Uuid(supersededFactorySynths[digital].revised);});
    CHECK(found!=factories.end()); if (found==factories.end()) return;
    const auto design=*found;
    CHECK(design.role==RackRole::synth && design.revision==1);
    CHECK(design.units.size()==1 && design.units[0].module==module && design.units[0].algorithmRevision==2);
    CHECK(design.panel.controls.size()==8);
    CHECK(validateRackDesign(design,RackValidationMode::portableDesign));
    CHECK(airack::test::frozenFactoryRackDesigns()==factories);
    juce::MemoryBlock portable;CHECK(encodeRackDesign(design,portable));RackDesign decoded;CHECK(decodeRackDesign(portable.getData(),portable.getSize(),decoded));CHECK(decoded==design);
    std::cout << "REVISED_FACTORY " << design.name << ' ' << juce::SHA256(portable).toHexString() << '\n';
    CHECK(juce::SHA256(portable).toHexString()==supersededFactorySynths[digital].revisedSha256);
    SoundDocument original,placed; RackDifference delta; RackOperation operation;
    operation.design=design;operation.newInstance=juce::Uuid(instancePrefix+"1");operation.destination.position=1;
    CHECK(makeRackCandidate(original,operation,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
    CHECK(placed.personalities && placed.personalities->instances.size()==1);if(!placed.personalities || placed.personalities->instances.empty())return;
    const auto first=placed.personalities->instances.front();CHECK(first.design==design);CHECK(first.id!=design.id);
    const auto* binding=rackUnitBinding(first,design.units[0].id);CHECK(binding!=nullptr);if(!binding)return;
    CHECK(binding->slot!=0);
    CHECK(placed.compatibility.synth[binding->slot][static_cast<std::size_t>(module)]==2);
    for(const auto& value:design.units[0].defaults){const auto index=rackParameterIndex(module,binding->slot,value.key);CHECK(index.has_value());if(index)CHECK(placed.values[*index]==value.normalized);}
    CHECK(parameterPhysicalValue(placed.values,*rackParameterIndex(module,binding->slot,"enabled"))==1);
    CHECK(parameterPhysicalValue(placed.values,*rackParameterIndex(module,binding->slot,"level"))>0);
    CHECK(parameterPhysicalValue(placed.values,*rackParameterIndex(module,binding->slot,"engine"))==(digital?1:0));
    for(std::size_t i=0;i<design.panel.controls.size();++i)CHECK(rackControlActive(placed,first,i));
    operation={};operation.kind=RackOperationKind::duplicate;operation.selected=first.id;operation.newInstance=juce::Uuid(instancePrefix+"2");operation.destination.position=2;
    SoundDocument duplicated;CHECK(makeRackCandidate(placed,operation,SoundEditOrigin::manual,duplicated,delta)==RackCandidateStatus::ready);
    CHECK(duplicated.personalities && duplicated.personalities->instances.size()==2);if(!duplicated.personalities || duplicated.personalities->instances.size()!=2)return;
    const auto& second=duplicated.personalities->instances.back();const auto* duplicateBinding=rackUnitBinding(second,design.units[0].id);CHECK(duplicateBinding!=nullptr);if(!duplicateBinding)return;
    CHECK(second.design==design && second.id!=first.id && duplicateBinding->slot!=binding->slot);
    CHECK(duplicated.compatibility.synth[duplicateBinding->slot]==duplicated.compatibility.synth[binding->slot]);
    const auto* primaryKey=digital?"wavetable.osc.1.position":"analog.filter.cutoff";
    const auto primary=*rackParameterIndex(module,binding->slot,primaryKey);
    const auto independent=*rackParameterIndex(module,duplicateBinding->slot,primaryKey);const auto before=duplicated.values[independent];
    duplicated.values[primary]=.23f;CHECK(duplicated.values[independent]==before);CHECK(duplicated.values[primary]!=before);
    CHECK(duplicated.personalities->instances.front().design==design && second.design==design);
    CHECK(validateSoundDocument(duplicated));juce::MemoryBlock sound;CHECK(encodeSoundState(duplicated,sound));SoundDocument recalled;CHECK(decodeSoundState(sound.getData(),sound.getSize(),recalled));CHECK(recalled==duplicated);
}
// A fresh catalog offers only the revised design under the shared name. Save Design and
// AI tweaks keep the design ID and bump its revision, so a later revision of the
// superseded lineage must coexist with the revised factory.
void checkSupersededCatalog(const RackDesign& revised,const RackDesign& superseded,const RackDesign& recalled,const char* savedKey) {
    const auto factories=airack::test::frozenFactoryRackDesigns();
    RackCatalogSnapshot fresh;RackCatalogStatus status=RackCatalogStatus::unavailable;
    CHECK(captureRackCatalog(factories,{},{},fresh,&status)&&status==RackCatalogStatus::ready);
    std::vector<const RackDesign*> offered;
    for(const auto& entry:fresh.entries)if(entry.definition.name==revised.name)offered.push_back(&entry.definition);
    CHECK(offered.size()==1&&*offered[0]==revised);
    auto saved=superseded;saved.revision=2;for(auto& value:saved.units[0].defaults)if(value.key==savedKey)value.normalized*=.5f;
    const std::vector<RackDesign> embedded{recalled,saved};
    RackCatalogSnapshot project;status=RackCatalogStatus::unavailable;
    CHECK(captureRackCatalog(factories,embedded,{},project,&status)&&status==RackCatalogStatus::ready);
    CHECK(project.entries.size()==factories.size()+2);
    const RackDesign* expected[]={&revised,&superseded,&saved};
    for(const auto* design:expected){
        const auto entry=std::find_if(project.entries.begin(),project.entries.end(),[&](const auto& e){return e.definition.id==design->id&&e.definition.revision==design->revision;});
        CHECK(entry!=project.entries.end()&&entry->definition==*design);
    }
}
void superseded_factory_synth_recall_and_catalog(bool digital) {
    begin(digital?"superseded_factory_digital_recall_and_catalog":"superseded_factory_analog_recall_and_catalog");
    const auto revised=revisedFactorySynth(digital);CHECK(revised.has_value());if(!revised)return;
    const auto module=digital?NativeModule::wavetable:NativeModule::analog;
    const auto superseded=supersededFactoryDesign(*revised,digital);
    juce::MemoryBlock bytes;CHECK(encodeRackDesign(superseded,bytes));
    CHECK(juce::SHA256(bytes).toHexString()==supersededFactorySynths[digital].supersededSha256);
    SoundDocument original,placed;RackOperation op;RackDifference delta;op.design=superseded;
    op.newInstance=juce::Uuid(digital?"91387715-13d3-4c57-a479-12251842b413":"91387715-13d3-4c57-a479-12251842a413");op.destination.position=1;
    CHECK(makeRackCandidate(original,op,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
    CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.empty())return;
    const auto* binding=rackUnitBinding(placed.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    juce::MemoryBlock state;CHECK(encodeSoundState(placed,state));SoundDocument recalled;
    CHECK(decodeSoundState(state.getData(),state.getSize(),recalled)&&recalled==placed);
    CHECK(recalled.personalities->instances[0].design==superseded);
    CHECK(recalled.compatibility.synth[binding->slot][static_cast<std::size_t>(module)]==legacyAlgorithmRevision);
    checkSupersededCatalog(*revised,superseded,recalled.personalities->instances[0].design,"level");
}
void superseded_factory_drive_recall_and_catalog() {
    begin("superseded_factory_drive_recall_and_catalog");
    const auto revised=factoryDesign(revisedCinderCoil);CHECK(revised.has_value());if(!revised)return;
    const auto superseded=supersededFactoryDrive(*revised);
    juce::MemoryBlock bytes;CHECK(encodeRackDesign(superseded,bytes));
    CHECK(juce::SHA256(bytes).toHexString()==supersededCinderCoilSha256);
    CHECK(!factoryDesign(supersededCinderCoil).has_value());
    SoundDocument original,placed;RackOperation op;RackDifference delta;op.design=superseded;
    op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842e413");op.destination={RackRole::fx,masterChain,0};
    CHECK(makeRackCandidate(original,op,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
    CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.empty())return;
    const auto* binding=rackUnitBinding(placed.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(placed.layout.effects[binding->slot].kind==EffectKind::drive);
    juce::MemoryBlock state;CHECK(encodeSoundState(placed,state));SoundDocument recalled;
    CHECK(decodeSoundState(state.getData(),state.getSize(),recalled)&&recalled==placed);
    CHECK(recalled.personalities->instances[0].design==superseded);
    CHECK(recalled.compatibility.effects[binding->slot][driveEffectBank]==legacyAlgorithmRevision);
    checkSupersededCatalog(*revised,superseded,recalled.personalities->instances[0].design,"drive.mix");
}
void superseded_factory_compressor_recall_and_catalog() {
    begin("superseded_factory_compressor_recall_and_catalog");
    const auto revised=factoryDesign(revisedAnchorPress);CHECK(revised.has_value());if(!revised)return;
    const auto superseded=supersededFactoryCompressor(*revised);
    juce::MemoryBlock bytes;CHECK(encodeRackDesign(superseded,bytes));
    CHECK(juce::SHA256(bytes).toHexString()==supersededAnchorPressSha256);
    CHECK(!factoryDesign(supersededAnchorPress).has_value());
    SoundDocument original,placed;RackOperation op;RackDifference delta;op.design=superseded;
    op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842d413");op.destination={RackRole::fx,masterChain,0};
    CHECK(makeRackCandidate(original,op,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
    CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.empty())return;
    const auto* binding=rackUnitBinding(placed.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(placed.layout.effects[binding->slot].kind==EffectKind::compressor);
    juce::MemoryBlock state;CHECK(encodeSoundState(placed,state));SoundDocument recalled;
    CHECK(decodeSoundState(state.getData(),state.getSize(),recalled)&&recalled==placed);
    CHECK(recalled.personalities->instances[0].design==superseded);
    CHECK(recalled.compatibility.effects[binding->slot][compressorEffectBank]==legacyAlgorithmRevision);
    checkSupersededCatalog(*revised,superseded,recalled.personalities->instances[0].design,"compressor.mix");
}
void superseded_factory_tap_recall_and_catalog(const SupersededFactoryTap& entry) {
    begin(entry.recallSection);
    if(const auto shipped=factoryDesign(entry.superseded)){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*shipped,bytes));std::cout << "SHIPPED_SUPERSEDED " << entry.name << ' ' << juce::SHA256(bytes).toHexString() << '\n';}
    const auto revised=factoryDesign(entry.revised);CHECK(revised.has_value());if(!revised)return;
    CHECK(revised->name==entry.name);const auto superseded=supersededFactoryTap(*revised,entry);
    juce::MemoryBlock bytes;CHECK(encodeRackDesign(superseded,bytes));
    CHECK(juce::SHA256(bytes).toHexString()==entry.supersededSha256);
    CHECK(!factoryDesign(entry.superseded).has_value());
    SoundDocument original,placed;RackOperation op;RackDifference delta;op.design=superseded;
    op.newInstance=juce::Uuid(entry.instance);op.destination={RackRole::fx,masterChain,0};
    CHECK(makeRackCandidate(original,op,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
    CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.empty())return;
    const auto* binding=rackUnitBinding(placed.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(placed.layout.effects[binding->slot].kind==entry.kind);
    juce::MemoryBlock state;CHECK(encodeSoundState(placed,state));SoundDocument recalled;
    CHECK(decodeSoundState(state.getData(),state.getSize(),recalled)&&recalled==placed);
    CHECK(recalled.personalities->instances[0].design==superseded);
    CHECK(recalled.compatibility.effects[binding->slot][entry.bank]==legacyAlgorithmRevision);
    checkSupersededCatalog(*revised,superseded,recalled.personalities->instances[0].design,entry.savedKey);
}
// Anchor Press's -54 dB threshold came from untouched Copper Arc at note 60
// (unity output, protection off, 2-4 s window), so the revised voices keep that level.
double sustainedRms(const RackDesign& design,bool digital,const RackDesign* insert=nullptr) {
    SoundDocument original,placed;RackOperation op;RackDifference delta;op.design=design;
    op.newInstance=juce::Uuid(digital?"91387715-13d3-4c57-a479-12251842b414":"91387715-13d3-4c57-a479-12251842a414");op.destination.position=0;
    op.allocation={{0,0}};op.allowSlotReuse=true;
    CHECK(makeRackCandidate(original,op,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
    if(insert){
        SoundDocument inserted;op={};op.design=*insert;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842e414");op.destination={RackRole::fx,masterChain,0};
        CHECK(makeRackCandidate(placed,op,SoundEditOrigin::manual,inserted,delta)==RackCandidateStatus::ready);placed=std::move(inserted);
    }
    setPhysical(placed,"global.master_level",1);setPhysical(placed,"output.gain_db",0);setPhysical(placed,"output.protection",0);
    RoutedPatch patch;RoutedSynthEngine engine;
    CHECK(makeRoutedPatch(placed,patch)&&engine.setPatch(patch)&&engine.prepare(48000,512));
    constexpr int rate=48000,block=512;double squares=0;
    juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;
    for(int offset=0;offset<4*rate;offset+=block){
        midi.clear();if(offset==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
        engine.render(audio,midi);
        for(int c=0;c<2;++c)for(int i=0;i<block;++i)if(offset+i>=2*rate)squares+=double(audio.getSample(c,i))*audio.getSample(c,i);
    }
    return std::sqrt(squares/(2.0*2*rate));
}
void factory_synth_sustain_matches_superseded(bool digital) {
    begin(digital?"factory_digital_sustain_matches_superseded":"factory_analog_sustain_matches_superseded");
    const auto revised=revisedFactorySynth(digital);CHECK(revised.has_value());if(!revised)return;
    const double before=sustainedRms(supersededFactoryDesign(*revised,digital),digital),after=sustainedRms(*revised,digital);
    const double difference=20*std::log10(after/before);
    std::cout << "FACTORY_SUSTAIN " << revised->name << " superseded_rms=" << before << " revised_rms=" << after << " difference_db=" << difference << '\n';
    CHECK(before>1e-3&&std::abs(difference)<=.25);
}
// In the same Copper Arc window, the revised drive keeps the superseded drive's level.
void factory_drive_sustain_matches_superseded() {
    begin("factory_drive_sustain_matches_superseded");
    const auto voice=revisedFactorySynth(false),revised=factoryDesign(revisedCinderCoil);CHECK(voice.has_value()&&revised.has_value());if(!voice||!revised)return;
    const auto superseded=supersededFactoryDrive(*revised);
    const double dry=sustainedRms(*voice,false),before=sustainedRms(*voice,false,&superseded),after=sustainedRms(*voice,false,&*revised);
    const double difference=20*std::log10(after/before);
    std::cout << "FACTORY_SUSTAIN " << revised->name << " dry_rms=" << dry << " superseded_rms=" << before << " revised_rms=" << after << " difference_db=" << difference << '\n';
    CHECK(before>1e-3&&std::abs(difference)<=.25);
}
// In the same window the revised compressor keeps the superseded one's gain reduction.
void factory_compressor_sustain_matches_superseded() {
    begin("factory_compressor_sustain_matches_superseded");
    const auto voice=revisedFactorySynth(false),revised=factoryDesign(revisedAnchorPress);CHECK(voice.has_value()&&revised.has_value());if(!voice||!revised)return;
    const auto superseded=supersededFactoryCompressor(*revised);
    const double dry=sustainedRms(*voice,false),before=sustainedRms(*voice,false,&superseded),after=sustainedRms(*voice,false,&*revised);
    const double difference=20*std::log10(after/before);
    std::cout << "FACTORY_SUSTAIN " << revised->name << " dry_rms=" << dry << " superseded_rms=" << before << " revised_rms=" << after
              << " superseded_reduction_db=" << 20*std::log10(dry/before) << " revised_reduction_db=" << 20*std::log10(dry/after) << " difference_db=" << difference << '\n';
    CHECK(before>1e-3&&before<dry&&std::abs(difference)<=.25);
}
// In the same window each revised modulated-tap design keeps the superseded one's level.
void factory_tap_sustain_matches_superseded(const SupersededFactoryTap& entry) {
    begin(entry.sustainSection);
    const auto voice=revisedFactorySynth(false),revised=factoryDesign(entry.revised);CHECK(voice.has_value()&&revised.has_value());if(!voice||!revised)return;
    const auto superseded=supersededFactoryTap(*revised,entry);
    const double dry=sustainedRms(*voice,false),before=sustainedRms(*voice,false,&superseded),after=sustainedRms(*voice,false,&*revised);
    const double difference=20*std::log10(after/before);
    std::cout << "FACTORY_SUSTAIN " << revised->name << " dry_rms=" << dry << " superseded_rms=" << before << " revised_rms=" << after << " difference_db=" << difference << '\n';
    CHECK(before>1e-3&&std::abs(difference)<=.25);
}
void factory_reverb_placement_and_recall() {
    begin("factory_reverb_placement_and_recall");
    const auto factories=airack::test::frozenFactoryRackDesigns();
    for(const auto& synth:supersededFactorySynths) {
        const auto factory=std::find_if(factories.begin(),factories.end(),[&](const auto& d){return d.id==juce::Uuid(synth.revised);});
        CHECK(factory!=factories.end());if(factory==factories.end())continue;
        juce::MemoryBlock bytes;CHECK(encodeRackDesign(*factory,bytes));CHECK(juce::SHA256(bytes).toHexString()==synth.revisedSha256);
    }
    const auto found=std::find_if(factories.begin(),factories.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842c401");});
    CHECK(found!=factories.end());if(found==factories.end())return;
    const auto design=*found;CHECK(design.role==RackRole::fx&&design.revision==1);
    CHECK(design.units.size()==1&&design.units[0].module==NativeModule::reverb&&design.units[0].algorithmRevision==2);
    CHECK(design.panel.controls.size()==5&&validateRackDesign(design,RackValidationMode::portableDesign));
    CHECK(airack::test::frozenFactoryRackDesigns()==factories);
    juce::MemoryBlock portable;CHECK(encodeRackDesign(design,portable));RackDesign decoded;
    CHECK(decodeRackDesign(portable.getData(),portable.getSize(),decoded)&&decoded==design);
    SoundDocument original,placed;RackOperation op;RackDifference delta;op.design=design;
    op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842c411");op.destination.role=RackRole::fx;op.destination.chain=masterChain;op.destination.position=0;
    CHECK(makeRackCandidate(original,op,SoundEditOrigin::manual,placed,delta)==RackCandidateStatus::ready);
    CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.empty())return;
    const auto first=placed.personalities->instances[0];const auto* binding=rackUnitBinding(first,0);CHECK(binding);if(!binding)return;
    CHECK(first.design==design&&placed.layout.effects[binding->slot].kind==EffectKind::reverb&&placed.compatibility.effects[binding->slot][2]==2);
    for(const auto& value:design.units[0].defaults){const auto index=rackParameterIndex(NativeModule::reverb,binding->slot,value.key);CHECK(index.has_value());if(index)CHECK(placed.values[*index]==value.normalized);}
    for(std::size_t i=0;i<design.panel.controls.size();++i)CHECK(rackControlActive(placed,first,i));
    op={};op.kind=RackOperationKind::duplicate;op.selected=first.id;op.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842c412");op.destination.role=RackRole::fx;op.destination.chain=masterChain;op.destination.position=1;
    SoundDocument duplicated;CHECK(makeRackCandidate(placed,op,SoundEditOrigin::manual,duplicated,delta)==RackCandidateStatus::ready);
    CHECK(duplicated.personalities&&duplicated.personalities->instances.size()==2);if(!duplicated.personalities||duplicated.personalities->instances.size()!=2)return;
    const auto& second=duplicated.personalities->instances[1];const auto* other=rackUnitBinding(second,0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&second.design==design&&second.id!=first.id);
    CHECK(duplicated.compatibility.effects[other->slot]==duplicated.compatibility.effects[binding->slot]);
    const auto primary=*rackParameterIndex(NativeModule::reverb,binding->slot,"reverb.size"),independent=*rackParameterIndex(NativeModule::reverb,other->slot,"reverb.size");
    const auto before=duplicated.values[independent];duplicated.values[primary]=.23f;CHECK(duplicated.values[independent]==before&&duplicated.values[primary]!=before);
    CHECK(duplicated.personalities->instances[0].design==design&&second.design==design);
    CHECK(validateSoundDocument(duplicated));juce::MemoryBlock sound;CHECK(encodeSoundState(duplicated,sound));SoundDocument recalled;
    CHECK(decodeSoundState(sound.getData(),sound.getSize(),recalled)&&recalled==duplicated);
}
// Task7: catch removed/changed catalog definitions and real placement/recall bugs.
void core_fx_preserves_existing_factory_definitions() {
    begin("core_fx_preserves_existing_factory_definitions");
    const auto factories=airack::test::frozenFactoryRackDesigns();
    constexpr const char* ids[]={supersededFactorySynths[0].revised,supersededFactorySynths[1].revised,"91387715-13d3-4c57-a479-12251842c401",revisedCinderCoil,revisedAnchorPress,
        supersededFactoryTaps[0].revised,supersededFactoryTaps[1].revised,supersededFactoryTaps[2].revised,supersededFactoryTaps[3].revised,supersededFactoryTaps[4].revised,supersededFactoryTaps[5].revised,supersededFactoryTaps[6].revised};
    constexpr const char* hashes[]={supersededFactorySynths[0].revisedSha256,supersededFactorySynths[1].revisedSha256,"f94fe9030a99900edf91ee7ff1f521d20dbba6d6db9bdc09774bd97ea7be4350",revisedCinderCoilSha256,revisedAnchorPressSha256,
        supersededFactoryTaps[0].revisedSha256,supersededFactoryTaps[1].revisedSha256,supersededFactoryTaps[2].revisedSha256,supersededFactoryTaps[3].revisedSha256,supersededFactoryTaps[4].revisedSha256,supersededFactoryTaps[5].revisedSha256,supersededFactoryTaps[6].revisedSha256};
    for(std::size_t i=0;i<std::size(ids);++i) {
        const auto found=std::find_if(factories.begin(),factories.end(),[&](const auto& d){return d.id==juce::Uuid(ids[i]);});
        CHECK(found!=factories.end());if(found==factories.end())continue;
        juce::MemoryBlock bytes;CHECK(encodeRackDesign(*found,bytes));CHECK(juce::SHA256(bytes).toHexString()==hashes[i]);
    }
}
void factory_core_fx_placement_and_recall(NativeModule module) {
    const bool compressor=module==NativeModule::compressor,drive=module==NativeModule::drive;
    begin(compressor?"factory_compressor_placement_and_recall":drive?"factory_drive_placement_and_recall":"factory_delay_placement_and_recall");
    const juce::String prefix=compressor?"91387715-13d3-4c57-a479-12251842d4":drive?"91387715-13d3-4c57-a479-12251842e4":"91387715-13d3-4c57-a479-12251842f4";
    const auto factories=airack::test::frozenFactoryRackDesigns();
    const auto found=std::find_if(factories.begin(),factories.end(),[&](const auto& d){
        return d.id==juce::Uuid(compressor?revisedAnchorPress:drive?revisedCinderCoil:supersededFactoryTaps[3].revised);});
    // Exactly one causal availability failure per missing default; no placeholder downstream work.
    CHECK(found!=factories.end());if(found==factories.end())return;
    const auto design=*found;
    CHECK(design.name==(compressor?"Anchor Press":drive?"Cinder Coil":"Echo Thread"));
    CHECK(design.role==RackRole::fx&&design.revision==1);
    CHECK(design.units.size()==1);if(design.units.size()!=1)return;
    const auto& unit=design.units[0];
    CHECK(unit.id==0&&unit.module==module&&unit.algorithmRevision==(drive?modernDriveAlgorithmRevision:compressor?modernCompressorAlgorithmRevision:modernDelayAlgorithmRevision));
    CHECK(!unit.defaults.empty()&&!design.panel.controls.empty());
    CHECK(validateRackDesign(design,RackValidationMode::portableDesign));
    juce::MemoryBlock portable;CHECK(encodeRackDesign(design,portable));RackDesign decoded;
    CHECK(decodeRackDesign(portable.getData(),portable.getSize(),decoded)&&decoded==design);
    std::cout << "REVISED_FACTORY " << design.name << ' ' << juce::SHA256(portable).toHexString() << '\n';
    CHECK(juce::SHA256(portable).toHexString()==(drive?revisedCinderCoilSha256:compressor?revisedAnchorPressSha256:supersededFactoryTaps[3].revisedSha256));
    CHECK(airack::test::frozenFactoryRackDesigns()==factories);
    SoundDocument original,placed;RackOperation op;RackDifference difference;
    // A real supported inactive Reverb2 bank must survive placement of the insert.
    original.compatibility.effects[0][2]=2;CHECK(validateSoundDocument(original));
    op.design=design;op.newInstance=juce::Uuid(prefix+"11");op.destination.role=RackRole::fx;op.destination.chain=masterChain;op.destination.position=0;
    CHECK(makeRackCandidate(original,op,SoundEditOrigin::manual,placed,difference)==RackCandidateStatus::ready);
    CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.size()!=1)return;
    const auto first=placed.personalities->instances[0];const auto* binding=rackUnitBinding(first,unit.id);CHECK(binding);if(!binding)return;
    const auto slot=binding->slot;const auto kind=compressor?EffectKind::compressor:drive?EffectKind::drive:EffectKind::delay;
    CHECK(first.id==op.newInstance&&first.id!=design.id&&first.design==design);
    CHECK(slot==0&&placed.layout.effects[slot].use==SlotUse::present&&placed.layout.effects[slot].kind==kind&&placed.layout.effects[slot].chain==masterChain);
    CHECK(placed.layout.effectCount[masterChain]==1&&placed.layout.effectOrder[masterChain][0]==slot);
    auto banks=original.compatibility.effects[slot];banks[static_cast<std::size_t>(module)-2]=unit.algorithmRevision;
    CHECK(placed.compatibility.effects[slot]==banks);
    for(const auto& value:unit.defaults) {
        const auto index=rackParameterIndex(module,slot,value.key);CHECK(index.has_value());
        if(index)CHECK(placed.values[*index]==value.normalized);
    }
    for(std::size_t i=0;i<design.panel.controls.size();++i) {
        const auto& control=design.panel.controls[i];CHECK(control.unit==unit.id&&rackControlActive(placed,first,i));
        CHECK(rackParameterIndex(module,slot,control.key).has_value());
    }
    // Focused controls must not hide the full ordinary native parameter surface.
    const std::vector<juce::String> keys=compressor?std::vector<juce::String>{"bypassed","compressor.threshold_db","compressor.ratio","compressor.attack_ms","compressor.release_ms","compressor.makeup_db","compressor.mix"}
        :drive?std::vector<juce::String>{"bypassed","drive.gain_db","drive.shape","drive.tone_hz","drive.mix"}
        :std::vector<juce::String>{"bypassed","delay.synced","delay.seconds","delay.beats","delay.feedback","delay.stereo","delay.filter_hz","delay.mix"};
    for(const auto& key:keys) {
        const auto index=rackParameterIndex(module,slot,key);CHECK(index.has_value());
        if(index)CHECK(parameterRegistry()[*index].id=="effect."+juce::String(int(slot)+1)+"."+key);
    }
    auto physical=[&](const juce::String& key) {
        const auto index=rackParameterIndex(module,slot,key);CHECK(index.has_value());
        return index?parameterPhysicalValue(placed.values,*index):std::numeric_limits<float>::quiet_NaN();
    };
    CHECK(physical("bypassed")==0);
    if(compressor) {
        // Threshold is calibrated from actual Copper Arc audio later, never guessed here.
        const auto threshold=physical("compressor.threshold_db");CHECK(std::isfinite(threshold)&&threshold>=-60&&threshold<0);
        // Actual untouched Copper Arc, unity/protectionOFF, fixed2-4s window:
        // frozen Task7 paired-input analysis selects -54dB for3.22652297dB GR on
        // compressor revision 1; revision 2 keeps that grip (factory_compressor_sustain_matches_superseded).
        CHECK(std::abs(threshold-revisedAnchorPressThresholdDb)<1e-5f);
        CHECK(std::abs(physical("compressor.ratio")-3)<1e-5f);
        CHECK(std::abs(physical("compressor.attack_ms")-15)<.001f);
        CHECK(std::abs(physical("compressor.release_ms")-150)<.001f);
        CHECK(physical("compressor.makeup_db")==0&&physical("compressor.mix")==1);
    } else if(drive) {
        CHECK(physical("drive.gain_db")>0&&physical("drive.gain_db")<=24);
        CHECK(physical("drive.mix")>0&&physical("drive.mix")<=1);
    } else {
        CHECK(physical("delay.seconds")>=.001f&&physical("delay.seconds")<=4);
        CHECK(physical("delay.feedback")>0&&physical("delay.feedback")<=.95f);
        CHECK(physical("delay.mix")>0&&physical("delay.mix")<=1);
    }
    op={};op.kind=RackOperationKind::duplicate;op.selected=first.id;op.newInstance=juce::Uuid(prefix+"12");op.destination.role=RackRole::fx;op.destination.chain=masterChain;op.destination.position=1;
    SoundDocument duplicated;CHECK(makeRackCandidate(placed,op,SoundEditOrigin::manual,duplicated,difference)==RackCandidateStatus::ready);
    CHECK(duplicated.personalities&&duplicated.personalities->instances.size()==2);if(!duplicated.personalities||duplicated.personalities->instances.size()!=2)return;
    const auto& second=duplicated.personalities->instances[1];const auto* other=rackUnitBinding(second,unit.id);CHECK(other);if(!other)return;
    CHECK(second.id==op.newInstance&&second.id!=first.id&&other->slot!=slot&&second.design==design);
    CHECK(duplicated.layout.effectCount[masterChain]==2&&duplicated.layout.effectOrder[masterChain][0]==slot&&duplicated.layout.effectOrder[masterChain][1]==other->slot);
    CHECK(duplicated.compatibility.effects[other->slot]==duplicated.compatibility.effects[slot]);
    const juce::String primary=compressor?"compressor.threshold_db":drive?"drive.gain_db":"delay.seconds";
    const auto a=rackParameterIndex(module,slot,primary),b=rackParameterIndex(module,other->slot,primary);
    CHECK(a.has_value()&&b.has_value());if(!a||!b)return;
    const auto before=duplicated.values[*b];const float edited=before<.5f?.83f:.17f;
    duplicated.values[*a]=edited;CHECK(duplicated.values[*a]!=before&&duplicated.values[*b]==before);
    CHECK(duplicated.personalities->instances[0].design==design&&second.design==design);
    CHECK(validateSoundDocument(duplicated));juce::MemoryBlock sound;CHECK(encodeSoundState(duplicated,sound));SoundDocument recalled;
    CHECK(decodeSoundState(sound.getData(),sound.getSize(),recalled)&&recalled==duplicated);
    juce::MemoryBlock unchanged;CHECK(encodeRackDesign(design,unchanged)&&unchanged==portable);
    const auto current=airack::test::frozenFactoryRackDesigns();const auto same=std::find_if(current.begin(),current.end(),[&](const auto& d){return d.id==design.id;});
    CHECK(same!=current.end());if(same!=current.end())CHECK(*same==design);
}
void modern_reverb_portable_admission() {
    begin("modern_reverb_portable_admission");
    CHECK(supportsAlgorithm(NativeModule::reverb,2));
    if(!supportsAlgorithm(NativeModule::reverb,2)) return;
    auto d=good(); d.role=RackRole::fx; d.units={{0,NativeModule::reverb,{{0,"reverb.mix",1}},2}};
    d.panel.controls={{"Mix",0,0,"reverb.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
    CHECK(validateRackDesign(d,RackValidationMode::portableDesign));
    juce::MemoryBlock bytes; CHECK(encodeRackDesign(d,bytes)); RackDesign out;
    CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),out)&&out==d);
    auto invalid=d; invalid.units[0].algorithmRevision=3; bad(invalid);
    const auto choices=rackDesignSchema()["properties"]["units"]["items"]["anyOf"];
    CHECK(choices.isArray()&&choices.size()==22);
    if(choices.isArray()) for(const auto& choice:*choices.getArray()) {
        const auto properties=choice["properties"];
        CHECK(properties["module"]["enum"].size()==1);
        const auto module=properties["module"]["enum"][0].toString();
        CHECK(int(properties["algorithm_revision"]["minimum"])==1);
        CHECK(int(properties["algorithm_revision"]["maximum"])==(module=="drive"||module=="delay"||module=="reverb"||module=="compressor"||module=="eq"||module=="filter_fx"||module=="chorus"||module=="flanger"||module=="dimension"||module=="frequency_split"||module=="analog"||module=="wavetable"?2:1));
        CHECK(!bool(choice["additionalProperties"]));
    }
}
void effect_eq_old_registry_and_state_evidence()
{
    begin("effect_eq_old_registry_and_state_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_EQ_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);CHECK(folder.createDirectory().wasOk());
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(759)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old759-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);
    else{CHECK(parameterCount==759);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<759;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(auto& banks:old.compatibility.effects)banks[2]=2;
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound5-nonuniform-inactive-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;CHECK(parameterCount==759&&encodeSoundState(old,bytes));CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;
    CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<759;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    for(auto& banks:recalled.compatibility.effects)CHECK(banks[2]==2);
}
void effect_eq_factory_and_control_authority()
{
    begin("effect_eq_factory_and_control_authority");
    CHECK(parameterCount>=831);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id(supersededFactoryTaps[4].revised);
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found==designs.end())return;
    CHECK(found->name=="Contour Loom"&&found->revision==1&&found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(6)&&found->units[0].algorithmRevision==modernEqAlgorithmRevision);
    CHECK(validateRackDesign(*found,RackValidationMode::portableDesign));
    {juce::MemoryBlock portable;CHECK(encodeRackDesign(*found,portable));std::cout << "REVISED_FACTORY " << found->name << ' ' << juce::SHA256(portable).toHexString() << '\n';
     CHECK(juce::SHA256(portable).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    for(const char* band:{"low_shelf","bell","high_shelf"})for(const char* field:{"frequency_hz","gain_db","q"}){
        const auto key="eq."+juce::String(band)+"."+field;
        for(std::uint8_t slot=0;slot<8;++slot){const auto index=rackParameterIndex(static_cast<NativeModule>(6),slot,key);CHECK(index&&*index>=759&&*index<831);}
    }
    juce::MemoryBlock bytes;CHECK(encodeRackDesign(*found,bytes));RackDesign out;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),out)&&out==*found);
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation operation;operation.kind=RackOperationKind::place;operation.design=*found;
    operation.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a811");operation.destination.role=RackRole::fx;
    operation.destination.chain=masterChain;operation.destination.position=0;
    SoundDocument placed;RackDifference diff;CHECK(makeRackCandidate(base,operation,SoundEditOrigin::manual,placed,diff)==RackCandidateStatus::ready);
    CHECK(placed.personalities&&placed.personalities->instances.size()==1);if(!placed.personalities||placed.personalities->instances.size()!=1)return;
    const auto first=placed.personalities->instances[0];const auto* binding=rackUnitBinding(first,0);CHECK(binding);if(!binding)return;
    CHECK(placed.layout.effects[binding->slot].kind==EffectKind::eq&&placed.compatibility.effects[binding->slot][eqEffectBank]==modernEqAlgorithmRevision);
    for(const auto& defaultValue:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::eq,binding->slot,defaultValue.key);
        CHECK(index&&placed.values[*index]==defaultValue.normalized);}
    operation={};operation.kind=RackOperationKind::duplicate;operation.selected=first.id;
    operation.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842a812");operation.destination.role=RackRole::fx;
    operation.destination.chain=masterChain;operation.destination.position=1;
    SoundDocument duplicate;CHECK(makeRackCandidate(placed,operation,SoundEditOrigin::manual,duplicate,diff)==RackCandidateStatus::ready);
    CHECK(duplicate.personalities&&duplicate.personalities->instances.size()==2);if(!duplicate.personalities||duplicate.personalities->instances.size()!=2)return;
    const auto& second=duplicate.personalities->instances[1];const auto* other=rackUnitBinding(second,0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&second.design==*found&&duplicate.compatibility.effects[other->slot]==duplicate.compatibility.effects[binding->slot]);
    // The ninth ordinary control is addressable without expanding the focused
    // face. Editing it must preserve the duplicate and embedded defaults.
    const auto highQ=rackParameterIndex(NativeModule::eq,binding->slot,"eq.high_shelf.q");
    const auto duplicateQ=rackParameterIndex(NativeModule::eq,other->slot,"eq.high_shelf.q");CHECK(highQ&&duplicateQ);
    if(highQ&&duplicateQ){const auto original=duplicate.values[*duplicateQ];duplicate.values[*highQ]=.83f;
        CHECK(duplicate.values[*duplicateQ]==original&&duplicate.personalities->instances[0].design==*found);}
    juce::MemoryBlock sound;CHECK(encodeSoundState(duplicate,sound));SoundDocument recalled;
    CHECK(decodeSoundState(sound.getData(),sound.getSize(),recalled)&&recalled==duplicate);

}
void filter_old_831_evidence()
{
    begin("filter_old_831_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_FILTER_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);CHECK(folder.createDirectory().wasOk());
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(831)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old831-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);
    else{CHECK(parameterCount==831);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<831;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(auto& banks:old.compatibility.effects)banks[2]=2;
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound6-nonuniform-inactive-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;CHECK(parameterCount==831&&encodeSoundState(old,bytes));CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;
    CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<831;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    for(const auto& banks:recalled.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1);
}
void effect_filter_factory_availability()
{
    begin("effect_filter_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id(supersededFactoryTaps[5].revised);
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found==designs.end())return;
    CHECK(found->name=="Veil Current"&&found->revision==1&&found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(7)&&found->units[0].algorithmRevision==modernFilterAlgorithmRevision);
    {juce::MemoryBlock portable;CHECK(encodeRackDesign(*found,portable));std::cout << "REVISED_FACTORY " << found->name << ' ' << juce::SHA256(portable).toHexString() << '\n';
     CHECK(juce::SHA256(portable).toHexString()==supersededFactoryTaps[5].revisedSha256);}
}
void effect_filter_actual_placement_and_recall()
{
    begin("effect_filter_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[5].revised);});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Veil Current"&&found->role==RackRole::fx&&found->units.size()==1&&found->units[0].algorithmRevision==modernFilterAlgorithmRevision&&validateRackDesign(*found,RackValidationMode::portableDesign));
    for(const char* key:{"filter_fx.mode","filter_fx.cutoff_hz","filter_fx.resonance","filter_fx.drive_db","filter_fx.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::filter,slot,key);CHECK(index&&*index>=831&&*index<871&&rackParameterActive(NativeModule::filter,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842b811");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::filter&&first.compatibility.effects[binding->slot][filterEffectBank]==modernFilterAlgorithmRevision);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::filter,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842b812");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::filter,binding->slot,"filter_fx.cutoff_hz"),b=rackParameterIndex(NativeModule::filter,other->slot,"filter_fx.cutoff_hz");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}

void effect_chorus_actual_placement_and_recall()
{
    begin("effect_chorus_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[0].revised);});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Orbit Ribbon"&&found->role==RackRole::fx&&found->revision==1&&found->units.size()==1&&found->units[0].algorithmRevision==modernModulatedTapAlgorithmRevision&&validateRackDesign(*found,RackValidationMode::portableDesign));
    {juce::MemoryBlock portable;CHECK(encodeRackDesign(*found,portable));std::cout << "REVISED_FACTORY " << found->name << ' ' << juce::SHA256(portable).toHexString() << '\n';
     CHECK(juce::SHA256(portable).toHexString()==supersededFactoryTaps[0].revisedSha256);}
    for(const char* key:{"chorus.rate_hz","chorus.depth","chorus.base_delay_ms","chorus.feedback","chorus.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::chorus,slot,key);CHECK(index&&*index>=871&&*index<911&&rackParameterActive(NativeModule::chorus,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842c811");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::chorus&&first.compatibility.effects[binding->slot][6]==modernModulatedTapAlgorithmRevision);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::chorus,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842c812");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::chorus,binding->slot,"chorus.base_delay_ms"),b=rackParameterIndex(NativeModule::chorus,other->slot,"chorus.base_delay_ms");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}


void chorus_old_871_evidence()
{
    begin("chorus_old_871_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_CHORUS_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);CHECK(folder.createDirectory().wasOk());
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(871)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old871-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);
    else{CHECK(parameterCount==871);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<871;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(auto& banks:old.compatibility.effects)banks[2]=2;
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound7-nonuniform-inactive-filter-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;CHECK(parameterCount==871&&encodeSoundState(old,bytes));CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;
    CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<871;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    for(const auto& banks:recalled.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1);

}
void effect_chorus_factory_availability()
{
    begin("effect_chorus_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id(supersededFactoryTaps[0].revised);
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found==designs.end())return;
    CHECK(found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(8)&&found->units[0].algorithmRevision==modernModulatedTapAlgorithmRevision);
}

void phaser_old_911_evidence()
{
    begin("phaser_old_911_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_PHASER_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);CHECK(folder.createDirectory().wasOk());
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(911)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old911-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);else{CHECK(parameterCount==911);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<911;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(auto& banks:old.compatibility.effects)banks[2]=2;
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound8-nonuniform-inactive-chorus-filter-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;CHECK(parameterCount==911&&encodeSoundState(old,bytes));CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<911;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    for(const auto& banks:recalled.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1);
}
void effect_phaser_actual_placement_and_recall()
{
    begin("effect_phaser_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842d901");});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Aster Sweep"&&found->role==RackRole::fx&&found->units.size()==1&&found->units[0].algorithmRevision==1&&validateRackDesign(*found,RackValidationMode::portableDesign));
    for(const char* key:{"phaser.rate_hz","phaser.depth","phaser.centre_hz","phaser.feedback","phaser.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::phaser,slot,key);CHECK(index&&*index>=911&&*index<951&&rackParameterActive(NativeModule::phaser,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842d911");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::phaser&&first.compatibility.effects[binding->slot][7]==1);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::phaser,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842d912");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::phaser,binding->slot,"phaser.centre_hz"),b=rackParameterIndex(NativeModule::phaser,other->slot,"phaser.centre_hz");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}
void effect_phaser_factory_availability()
{
    begin("effect_phaser_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id("91387715-13d3-4c57-a479-12251842d901");
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found!=designs.end())CHECK(found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(9)&&found->units[0].algorithmRevision==1);
}

void flanger_old_951_evidence()
{
    begin("flanger_old_951_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_FLANGER_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);CHECK(folder.createDirectory().wasOk());
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(951)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old951-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);else{CHECK(parameterCount==951);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<951;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(auto& banks:old.compatibility.effects)banks[2]=2;
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound9-nonuniform-inactive-phaser-chorus-filter-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;CHECK(parameterCount==951&&encodeSoundState(old,bytes));CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<951;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    for(const auto& banks:recalled.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1&&banks[7]==1);
}
void effect_flanger_actual_placement_and_recall()
{
    begin("effect_flanger_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[1].revised);});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Silver Loop"&&found->role==RackRole::fx&&found->revision==1&&found->units.size()==1&&found->units[0].algorithmRevision==modernModulatedTapAlgorithmRevision&&validateRackDesign(*found,RackValidationMode::portableDesign));
    {juce::MemoryBlock portable;CHECK(encodeRackDesign(*found,portable));std::cout << "REVISED_FACTORY " << found->name << ' ' << juce::SHA256(portable).toHexString() << '\n';
     CHECK(juce::SHA256(portable).toHexString()==supersededFactoryTaps[1].revisedSha256);}
    for(const char* key:{"flanger.rate_hz","flanger.depth","flanger.base_delay_ms","flanger.feedback","flanger.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::flanger,slot,key);CHECK(index&&*index>=951&&*index<991&&rackParameterActive(NativeModule::flanger,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842e911");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::flanger&&first.compatibility.effects[binding->slot][8]==modernModulatedTapAlgorithmRevision);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::flanger,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842e912");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::flanger,binding->slot,"flanger.base_delay_ms"),b=rackParameterIndex(NativeModule::flanger,other->slot,"flanger.base_delay_ms");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}
void effect_flanger_factory_availability()
{
    begin("effect_flanger_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id(supersededFactoryTaps[1].revised);
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found!=designs.end())CHECK(found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(10)&&found->units[0].algorithmRevision==modernModulatedTapAlgorithmRevision);
}

void stereo_old_991_evidence()
{
    begin("stereo_old_991_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_STEREO_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);CHECK(folder.createDirectory().wasOk());
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(991)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old991-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);else{CHECK(parameterCount==991);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<991;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(auto& banks:old.compatibility.effects)banks[2]=2;
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound10-nonuniform-inactive-flanger-phaser-chorus-filter-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;CHECK(parameterCount==991&&encodeSoundState(old,bytes));CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<991;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    for(const auto& banks:recalled.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1&&banks[7]==1&&banks[8]==1);
}
void effect_stereo_actual_placement_and_recall()
{
    begin("effect_stereo_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842f901");});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Centre Line"&&found->role==RackRole::fx&&found->units.size()==1&&found->units[0].algorithmRevision==1&&validateRackDesign(*found,RackValidationMode::portableDesign));
    for(const char* key:{"stereo.gain_db","stereo.pan","stereo.width","stereo.channel_mode"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::stereo,slot,key);CHECK(index&&*index>=991&&*index<1023&&rackParameterActive(NativeModule::stereo,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842f911");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::stereo&&first.compatibility.effects[binding->slot][9]==1);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::stereo,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842f912");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::stereo,binding->slot,"stereo.width"),b=rackParameterIndex(NativeModule::stereo,other->slot,"stereo.width");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}
void effect_stereo_factory_availability()
{
    begin("effect_stereo_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id("91387715-13d3-4c57-a479-12251842f901");
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found!=designs.end()){
        CHECK(found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(11)&&found->units[0].algorithmRevision==1);
        for(const char* key:{"stereo.gain_db","stereo.pan","stereo.width","stereo.channel_mode"})for(std::uint8_t slot=0;slot<8;++slot){
            const auto index=rackParameterIndex(static_cast<NativeModule>(11),slot,key);CHECK(index&&*index>=991&&*index<1023&&rackParameterActive(static_cast<NativeModule>(11),key));}
    }
}

void dimension_old_1023_evidence()
{
    begin("dimension_old_1023_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_DIMENSION_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);CHECK(folder.createDirectory().wasOk());
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(1023)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old1023-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);else{CHECK(parameterCount==1023);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<1023;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(auto& banks:old.compatibility.effects)banks[2]=2;
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound11-nonuniform-inactive-stereo-flanger-phaser-chorus-filter-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;CHECK(parameterCount==1023&&encodeSoundState(old,bytes));CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<1023;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    for(const auto& banks:recalled.compatibility.effects)CHECK(banks[2]==2&&banks[4]==1&&banks[5]==1&&banks[6]==1&&banks[7]==1&&banks[8]==1&&banks[9]==1);
}
void effect_frequency_shift_actual_placement_and_recall()
{
    begin("effect_frequency_shift_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bb01");});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Sideband Tide"&&found->role==RackRole::fx&&found->units.size()==1&&found->units[0].algorithmRevision==1&&validateRackDesign(*found,RackValidationMode::portableDesign));
    for(const char* key:{"frequency_shift.hz","frequency_shift.stereo_offset_hz","frequency_shift.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::frequencyShift,slot,key);CHECK(index&&*index>=1055&&*index<1079&&rackParameterActive(NativeModule::frequencyShift,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bb11");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::frequencyShift&&first.compatibility.effects[binding->slot][11]==1);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::frequencyShift,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bb12");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::frequencyShift,binding->slot,"frequency_shift.mix"),b=rackParameterIndex(NativeModule::frequencyShift,other->slot,"frequency_shift.mix");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}
void effect_frequency_split_actual_placement_and_recall()
{
    begin("effect_frequency_split_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[6].revised);});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Band Balance"&&found->role==RackRole::fx&&found->units.size()==1&&found->units[0].algorithmRevision==modernFrequencySplitAlgorithmRevision&&validateRackDesign(*found,RackValidationMode::portableDesign));
    for(const char* key:{"frequency_split.crossover_hz","frequency_split.low_gain_db","frequency_split.low_mute","frequency_split.low_tone","frequency_split.high_gain_db","frequency_split.high_mute","frequency_split.high_tone","frequency_split.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::frequencySplit,slot,key);CHECK(index&&*index>=1111&&*index<1175&&rackParameterActive(NativeModule::frequencySplit,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bd11");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::frequencySplit&&first.compatibility.effects[binding->slot][frequencySplitEffectBank]==modernFrequencySplitAlgorithmRevision);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::frequencySplit,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bd12");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::frequencySplit,binding->slot,"frequency_split.mix"),b=rackParameterIndex(NativeModule::frequencySplit,other->slot,"frequency_split.mix");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}
void effect_convolution_actual_placement_and_recall()
{
    begin("effect_convolution_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bc01");});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Ember Room"&&found->role==RackRole::fx&&found->units.size()==1&&found->units[0].algorithmRevision==1&&validateRackDesign(*found,RackValidationMode::portableDesign));
    for(const char* key:{"convolution.ir","convolution.predelay_ms","convolution.wet_trim_db","convolution.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::convolution,slot,key);CHECK(index&&*index>=1079&&*index<1111&&rackParameterActive(NativeModule::convolution,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bc11");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::convolution&&first.compatibility.effects[binding->slot][12]==1);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::convolution,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842bc12");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::convolution,binding->slot,"convolution.mix"),b=rackParameterIndex(NativeModule::convolution,other->slot,"convolution.mix");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}
void effect_dimension_actual_placement_and_recall()
{
    begin("effect_dimension_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[2].revised);});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Twin Current"&&found->role==RackRole::fx&&found->revision==1&&found->units.size()==1&&found->units[0].algorithmRevision==modernModulatedTapAlgorithmRevision&&validateRackDesign(*found,RackValidationMode::portableDesign));
    {juce::MemoryBlock portable;CHECK(encodeRackDesign(*found,portable));std::cout << "REVISED_FACTORY " << found->name << ' ' << juce::SHA256(portable).toHexString() << '\n';
     CHECK(juce::SHA256(portable).toHexString()==supersededFactoryTaps[2].revisedSha256);}
    for(const char* key:{"dimension.depth","dimension.rate_hz","dimension.width","dimension.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::dimension,slot,key);CHECK(index&&*index>=1023&&*index<1055&&rackParameterActive(NativeModule::dimension,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842aa11");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::dimension&&first.compatibility.effects[binding->slot][10]==modernModulatedTapAlgorithmRevision);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::dimension,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842aa12");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::dimension,binding->slot,"dimension.width"),b=rackParameterIndex(NativeModule::dimension,other->slot,"dimension.width");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}
void effect_dimension_factory_availability()
{
    begin("effect_dimension_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id(supersededFactoryTaps[2].revised);
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found!=designs.end()){
        CHECK(found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(12)&&found->units[0].algorithmRevision==modernModulatedTapAlgorithmRevision);
        for(const char* key:{"dimension.depth","dimension.rate_hz","dimension.width","dimension.mix"})for(std::uint8_t slot=0;slot<8;++slot){
            const auto index=rackParameterIndex(static_cast<NativeModule>(12),slot,key);CHECK(index&&*index>=1023&&*index<1055&&rackParameterActive(static_cast<NativeModule>(12),key));}
    }
}

void frequency_shift_old_1055_evidence()
{
    begin("frequency_shift_old_1055_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_FREQUENCY_SHIFT_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);const auto directoryReady=folder.createDirectory().wasOk();CHECK(directoryReady);if(!directoryReady)return;
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(1055)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old1055-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);else{CHECK(parameterCount==1055);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<1055;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(std::size_t slot=0;slot<8;++slot)old.compatibility.effects[slot][2]=static_cast<AlgorithmRevision>(1+slot%2);
    for(std::size_t i=0;i<8;++i)old.macros[i]={registry[1023+i].stateKey,-.7f+.15f*float(i)};
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound12-nonuniform-inactive-dimension-stereo-flanger-phaser-chorus-filter-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;CHECK(parameterCount==1055&&encodeSoundState(old,bytes));CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<1055;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    CHECK(recalled==old);
}
void effect_frequency_shift_factory_availability()
{
    begin("effect_frequency_shift_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id("91387715-13d3-4c57-a479-12251842bb01");
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found!=designs.end()){
        CHECK(found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(13)&&found->units[0].algorithmRevision==1);
        for(const char* key:{"frequency_shift.hz","frequency_shift.stereo_offset_hz","frequency_shift.mix"})for(std::uint8_t slot=0;slot<8;++slot){
            const auto index=rackParameterIndex(static_cast<NativeModule>(13),slot,key);CHECK(index&&*index>=1055&&*index<1079&&rackParameterActive(static_cast<NativeModule>(13),key));}
    }
}

void convolution_old_1079_evidence()
{
    begin("convolution_old_1079_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_CONVOLUTION_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);const auto directoryReady=folder.createDirectory().wasOk();CHECK(directoryReady);if(!directoryReady)return;
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(1079)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old1079-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);else{CHECK(parameterCount==1079);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<1079;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(std::size_t slot=0;slot<8;++slot)old.compatibility.effects[slot][2]=static_cast<AlgorithmRevision>(1+slot%2);
    for(std::size_t i=0;i<8;++i)old.macros[i]={registry[1055+i].stateKey,-.7f+.15f*float(i)};
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound13-nonuniform-inactive-frequency-shift-dimension-stereo-flanger-phaser-chorus-filter-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;const bool encoded=parameterCount==1079&&encodeSoundState(old,bytes);CHECK(encoded);if(!encoded)return;CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<1079;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    CHECK(recalled==old);
}
void effect_convolution_factory_availability()
{
    begin("effect_convolution_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id("91387715-13d3-4c57-a479-12251842bc01");
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found!=designs.end()){
        CHECK(found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(14)&&found->units[0].algorithmRevision==1);
        for(const char* key:{"convolution.ir","convolution.predelay_ms","convolution.wet_trim_db","convolution.mix"})for(std::uint8_t slot=0;slot<8;++slot){
            const auto index=rackParameterIndex(static_cast<NativeModule>(14),slot,key);CHECK(index&&*index>=1079&&*index<1111&&rackParameterActive(static_cast<NativeModule>(14),key));}
    }
}

void frequency_split_old_1111_evidence()
{
    begin("frequency_split_old_1111_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_FREQUENCY_SPLIT_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);const auto directoryReady=folder.createDirectory().wasOk();CHECK(directoryReady);if(!directoryReady)return;
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(1111)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old1111-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);else{CHECK(parameterCount==1111);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<1111;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(std::size_t slot=0;slot<8;++slot)old.compatibility.effects[slot][2]=static_cast<AlgorithmRevision>(1+slot%2);
    for(std::size_t i=0;i<8;++i){const auto target=1079+4*(i/3)+1+(i%3);CHECK(registry[target].kind==ParameterKind::continuous);old.macros[i]={registry[target].stateKey,-.7f+.15f*float(i)};}
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound14-nonuniform-inactive-convolution-frequency-shift-dimension-stereo-flanger-phaser-chorus-filter-eq-reverb2.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;const bool encoded=parameterCount==1111&&encodeSoundState(old,bytes);CHECK(encoded);if(!encoded)return;CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<1111;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    CHECK(recalled==old);
}
void effect_frequency_split_factory_availability()
{
    begin("effect_frequency_split_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id(supersededFactoryTaps[6].revised);
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found==designs.end())return;
    CHECK(found->name=="Band Balance"&&found->revision==1&&found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(15)&&found->units[0].algorithmRevision==modernFrequencySplitAlgorithmRevision);
    for(const char* key:{"frequency_split.crossover_hz","frequency_split.low_gain_db","frequency_split.low_mute","frequency_split.low_tone","frequency_split.high_gain_db","frequency_split.high_mute","frequency_split.high_tone","frequency_split.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(static_cast<NativeModule>(15),slot,key);CHECK(index&&*index>=1111&&*index<1175&&rackParameterActive(static_cast<NativeModule>(15),key));}
    {juce::MemoryBlock portable;CHECK(encodeRackDesign(*found,portable));std::cout << "REVISED_FACTORY " << found->name << ' ' << juce::SHA256(portable).toHexString() << '\n';
     CHECK(juce::SHA256(portable).toHexString()==supersededFactoryTaps[6].revisedSha256);}
}

void mid_side_split_old_1175_evidence()
{
    begin("mid_side_split_old_1175_evidence");
    const auto directory=juce::SystemStats::getEnvironmentVariable("AIRACK_MID_SIDE_SPLIT_OLD_EVIDENCE",{});
    if(directory.isEmpty())return;
    const juce::File folder(directory);const auto directoryReady=folder.createDirectory().wasOk();CHECK(directoryReady);if(!directoryReady)return;
    juce::Array<juce::var> rows;const auto registry=parameterRegistry();
    for(const auto& d:registry.first(1175)){
        juce::Array<juce::var> choices;for(const auto& c:d.choices)choices.add(c);
        rows.add(juce::Array<juce::var>{d.id,d.name,d.unit,int(d.kind),double(d.minimum),double(d.maximum),double(d.defaultValue),double(d.skew),juce::String(std::to_string(d.stateKey)),choices});
    }
    const auto text=juce::JSON::toString(rows,true,17);const auto file=folder.getChildFile("old1175-descriptors.json");
    if(file.existsAsFile())CHECK(file.loadFileAsString()==text);else{CHECK(parameterCount==1175);CHECK(file.replaceWithText(text));}
    SoundDocument old;for(std::size_t i=0;i<1175;++i){if(registry[i].kind==ParameterKind::continuous)old.values[i]=float((i*13)%71)/100;old.controlLocks[i]=(i%17)==0;}
    for(std::size_t slot=0;slot<8;++slot)old.compatibility.effects[slot][2]=static_cast<AlgorithmRevision>(1+slot%2);
    for(std::size_t i=0;i<8;++i){const std::array<std::size_t,6> fields{{0,1,3,4,6,7}};const auto target=1111+8*(i/6)+fields[i%6];CHECK(registry[target].kind==ParameterKind::continuous);old.macros[i]={registry[target].stateKey,-.7f+.15f*float(i)};}
    CHECK(validateSoundDocument(old));const auto state=folder.getChildFile("old-sound15-nonuniform-inactive-all-fx.soundstate");
    if(!state.existsAsFile()){juce::MemoryBlock bytes;const bool encoded=parameterCount==1175&&encodeSoundState(old,bytes);CHECK(encoded);if(!encoded)return;CHECK(state.replaceWithData(bytes.getData(),bytes.getSize()));}
    juce::MemoryBlock bytes;CHECK(state.loadFileAsData(bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled));
    for(std::size_t i=0;i<1175;++i)CHECK(recalled.values[i]==old.values[i]&&recalled.controlLocks[i]==old.controlLocks[i]);
    CHECK(recalled==old);
}
void effect_mid_side_split_actual_placement_and_recall()
{
    begin("effect_mid_side_split_actual_placement_and_recall");const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto eq=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid(supersededFactoryTaps[4].revised);});CHECK(eq!=designs.end());
    if(eq!=designs.end()){juce::MemoryBlock bytes;CHECK(encodeRackDesign(*eq,bytes));CHECK(juce::SHA256(bytes).toHexString()==supersededFactoryTaps[4].revisedSha256);}
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842be01");});CHECK(found!=designs.end());if(found==designs.end())return;
    CHECK(found->name=="Field Balance"&&found->role==RackRole::fx&&found->units.size()==1&&found->units[0].algorithmRevision==1&&validateRackDesign(*found,RackValidationMode::portableDesign));
    for(const char* key:{"mid_side_split.mid_gain_db","mid_side_split.mid_mute","mid_side_split.mid_tone","mid_side_split.side_gain_db","mid_side_split.side_mute","mid_side_split.side_tone","mid_side_split.mix"})for(std::uint8_t slot=0;slot<8;++slot){
        const auto index=rackParameterIndex(NativeModule::midSideSplit,slot,key);CHECK(index&&*index>=1175&&*index<1231&&rackParameterActive(NativeModule::midSideSplit,key));}
    SoundDocument base;for(auto& bank:base.compatibility.effects)bank[2]=2;
    RackOperation place;place.design=*found;place.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842be11");place.destination={RackRole::fx,masterChain,0};
    RackDifference difference;SoundDocument first;CHECK(makeRackCandidate(base,place,SoundEditOrigin::manual,first,difference)==RackCandidateStatus::ready);
    if(!first.personalities||first.personalities->instances.size()!=1){CHECK(false);return;}
    const auto* binding=rackUnitBinding(first.personalities->instances[0],0);CHECK(binding);if(!binding)return;
    CHECK(first.layout.effects[binding->slot].kind==EffectKind::midSideSplit&&first.compatibility.effects[binding->slot][14]==1);
    for(const auto& v:found->units[0].defaults){const auto index=rackParameterIndex(NativeModule::midSideSplit,binding->slot,v.key);CHECK(index&&first.values[*index]==v.normalized);}
    RackOperation duplicate;duplicate.kind=RackOperationKind::duplicate;duplicate.selected=place.newInstance;
    duplicate.newInstance=juce::Uuid("91387715-13d3-4c57-a479-12251842be12");duplicate.destination={RackRole::fx,masterChain,1};SoundDocument pair;
    CHECK(makeRackCandidate(first,duplicate,SoundEditOrigin::manual,pair,difference)==RackCandidateStatus::ready);if(!pair.personalities||pair.personalities->instances.size()!=2){CHECK(false);return;}
    const auto* other=rackUnitBinding(pair.personalities->instances[1],0);CHECK(other);if(!other)return;
    CHECK(other->slot!=binding->slot&&pair.compatibility.effects[other->slot]==pair.compatibility.effects[binding->slot]);
    const auto a=rackParameterIndex(NativeModule::midSideSplit,binding->slot,"mid_side_split.mix"),b=rackParameterIndex(NativeModule::midSideSplit,other->slot,"mid_side_split.mix");CHECK(a&&b);
    if(a&&b){const auto old=pair.values[*b];pair.values[*a]=.83f;CHECK(pair.values[*b]==old&&pair.personalities->instances[0].design==*found);}
    juce::MemoryBlock bytes;CHECK(encodeSoundState(pair,bytes));SoundDocument recalled;CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==pair);
    CHECK(encodeRackDesign(*found,bytes));RackDesign definition;CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),definition)&&definition==*found);
}
void effect_mid_side_split_factory_availability()
{
    begin("effect_mid_side_split_factory_availability");CHECK(parameterCount==4519);
    const auto designs=airack::test::frozenFactoryRackDesigns();const juce::Uuid id("91387715-13d3-4c57-a479-12251842be01");
    const auto found=std::find_if(designs.begin(),designs.end(),[&](const auto& d){return d.id==id;});CHECK(found!=designs.end());
    if(found!=designs.end()){
        CHECK(found->units.size()==1&&found->units[0].module==static_cast<NativeModule>(16)&&found->units[0].algorithmRevision==1);
        for(const char* key:{"mid_side_split.mid_gain_db","mid_side_split.mid_mute","mid_side_split.mid_tone","mid_side_split.side_gain_db","mid_side_split.side_mute","mid_side_split.side_tone","mid_side_split.mix"})for(std::uint8_t slot=0;slot<8;++slot){
            const auto index=rackParameterIndex(static_cast<NativeModule>(16),slot,key);CHECK(index&&*index>=1175&&*index<1231&&rackParameterActive(static_cast<NativeModule>(16),key));}
    }
}

// Development-only existing-codec calibration; no catalog implementation.
#if defined(_MSC_VER) && defined(_DEBUG)
namespace catalogCalibration {
// Opt-in diagnostic only: direct kernel writes do not use the CRT allocator.
bool denialTracing=false;
HANDLE denialTraceHandle=INVALID_HANDLE_VALUE;
_CRT_REPORT_HOOK previousDenialReport=nullptr;
template<std::size_t N> void denialMark(const char (&text)[N]) noexcept {
    if(denialTracing){DWORD written=0;WriteFile(denialTraceHandle,text,static_cast<DWORD>(N-1),&written,nullptr);}
}
int __cdecl denialReport(int type,char* text,int* result) {
    if(type==_CRT_WARN)denialMark("DENIAL_TRACE report-WARN-enter\n");
    else if(type==_CRT_ERROR)denialMark("DENIAL_TRACE report-ERROR-enter\n");
    else if(type==_CRT_ASSERT)denialMark("DENIAL_TRACE report-ASSERT-enter\n");
    else denialMark("DENIAL_TRACE report-other-enter\n");
    const auto handled=previousDenialReport?previousDenialReport(type,text,result):0;
    // This is the hook's return boundary, not return from default CRT reporting.
    denialMark("DENIAL_TRACE report-hook-return\n");return handled;
}
struct AllocationRow { long request=0;std::size_t bytes=0; };
struct HeapResult { std::size_t live=0,peak=0,reallocOverlapPeak=0,allocated=0,freed=0,baselineFreed=0,allocations=0,reallocations=0,denials=0;bool valid=true; };
// Fixed recorder storage is test infrastructure, outside measured CRT payload.
static std::array<AllocationRow,131072> rows;
thread_local bool recording=false,insideHook=false;
thread_local HeapResult heap;
thread_local std::size_t denyAt=0,allocationIndex=0;
AllocationRow* find(long request,bool insert) noexcept {
    if(request<=0){heap.valid=false;return nullptr;}
    auto index=static_cast<std::size_t>(request)&(rows.size()-1);
    for(std::size_t n=0;n<rows.size();++n,index=(index+1)&(rows.size()-1)){
        auto& row=rows[index];if(row.request==request)return &row;
        if(row.request==0)return insert?&row:nullptr;
    }
    if(insert)heap.valid=false;return nullptr;
}
void erase(AllocationRow* row) noexcept {
    constexpr auto mask=rows.size()-1;auto hole=static_cast<std::size_t>(row-rows.data());rows[hole]={};
    auto next=(hole+1)&mask;
    // Backshift the probe cluster; deleted slots do not accumulate tombstones.
    for(std::size_t n=0;n<rows.size()-1&&rows[next].request>0;++n,next=(next+1)&mask){
        const auto home=static_cast<std::size_t>(rows[next].request)&mask;
        if(((hole-home)&mask)<((next-home)&mask)){rows[hole]=rows[next];rows[next]={};hole=next;}
    }
}
void remove(void* data,int use) noexcept {
    if(!data)return;long request=0;
    const auto bytes=_msize_dbg(data,use);
    if(bytes>UINT_MAX||!_CrtIsMemoryBlock(data,static_cast<unsigned>(bytes),&request,nullptr,nullptr)){heap.valid=false;return;}
    if(auto* row=find(request,false)){heap.live-=row->bytes;heap.freed+=row->bytes;erase(row);}else heap.baselineFreed+=bytes;
}
int __cdecl hook(int kind,void* data,std::size_t bytes,int use,long request,const unsigned char*,int) {
    if(!recording||insideHook)return 1;insideHook=true;
    // FREE can report NORMAL for a stored CRT block; classify the actual header.
    if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC)){use=_CrtReportBlockType(data);if(use==-1)heap.valid=false;}
    use&=0xffff;if(use!=_NORMAL_BLOCK&&use!=_CLIENT_BLOCK){insideHook=false;return 1;}
    if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){
        if(++allocationIndex==denyAt){++heap.denials;insideHook=false;denialMark("DENIAL_TRACE alloc-hook-reject-before-return\n");return 0;}
        if(kind==_HOOK_REALLOC){++heap.reallocations;heap.reallocOverlapPeak=std::max(heap.reallocOverlapPeak,heap.live+bytes);remove(data,use);}
        ++heap.allocations;heap.allocated+=bytes;heap.live+=bytes;heap.peak=std::max(heap.peak,heap.live);
        if(auto* row=find(request,true))*row={request,bytes};
    }else if(kind==_HOOK_FREE)remove(data,use);
    insideHook=false;return 1;
}
template<class F> HeapResult observe(const char* label,F&& work,std::size_t deny=0) {
    rows.fill({});heap={};denyAt=deny;allocationIndex=0;
    std::cout<<"CATALOG_PHASE label="<<label<<std::endl;
    _CrtMemState before{},after{};denialMark("DENIAL_TRACE checkpoint-before-enter\n");_CrtMemCheckpoint(&before);
    denialMark("DENIAL_TRACE checkpoint-before-return\n");
    const auto started=juce::Time::getMillisecondCounterHiRes();
    denialMark("DENIAL_TRACE alloc-hook-install-before\n");const auto previous=_CrtSetAllocHook(hook);recording=true;
    denialMark("DENIAL_TRACE work-enter\n");
    try{work();}catch(...){denialMark("DENIAL_TRACE work-outer-catch\n");recording=false;_CrtSetAllocHook(previous);throw;}
    denialMark("DENIAL_TRACE work-return\n");recording=false;_CrtSetAllocHook(previous);
    denialMark("DENIAL_TRACE alloc-hook-restored\n");_CrtMemCheckpoint(&after);denialMark("DENIAL_TRACE checkpoint-after-return\n");
    const auto result=heap;const auto elapsed=juce::Time::getMillisecondCounterHiRes()-started;
    // Hook payload accounting excludes CRT guard/header overhead and other threads.
    // Realloc accounting is prospective; successful fixtures are cross-checked
    // against the final CRT payload delta, including warmed global pool retention.
    const auto normalDelta=static_cast<std::int64_t>(after.lSizes[_NORMAL_BLOCK])-static_cast<std::int64_t>(before.lSizes[_NORMAL_BLOCK]);
    const auto clientDelta=static_cast<std::int64_t>(after.lSizes[_CLIENT_BLOCK])-static_cast<std::int64_t>(before.lSizes[_CLIENT_BLOCK]);
    std::cout<<"CATALOG_HEAP label="<<label<<" peak_requested="<<result.peak<<" realloc_overlap_candidate="<<result.reallocOverlapPeak<<" retained_requested="<<result.live
             <<" allocated_requested="<<result.allocated<<" freed_requested="<<result.freed<<" allocations="<<result.allocations
             <<" baseline_freed_requested="<<result.baselineFreed<<" reallocations="<<result.reallocations<<" denials="<<result.denials<<" crt_normal_delta="<<normalDelta
             <<" crt_client_delta="<<clientDelta<<" elapsed_ms="<<elapsed<<" recorder_valid="<<result.valid<<std::endl;
    CHECK(result.valid);if(deny==0)CHECK(static_cast<std::int64_t>(result.live)-static_cast<std::int64_t>(result.baselineFreed)==normalDelta+clientDelta);
    return result;
}
void recorder_self_checks() {
    // Known payloads independently exercise real CRT type, baseline and realloc semantics.
    void* baseline=_malloc_dbg(80,_NORMAL_BLOCK,__FILE__,__LINE__);
    void* crtBaseline=_malloc_dbg(96,_CRT_BLOCK,__FILE__,__LINE__);CHECK(baseline&&crtBaseline);
    if(!baseline||!crtBaseline){if(baseline)_free_dbg(baseline,_NORMAL_BLOCK);if(crtBaseline)_free_dbg(crtBaseline,_NORMAL_BLOCK);return;}
    const auto known=observe("recorder-known-domain",[&]{
        void* small=_malloc_dbg(64,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(small);
        if(small){void* large=_realloc_dbg(small,128,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(large);_free_dbg(large?large:small,_NORMAL_BLOCK);}
        _free_dbg(baseline,_NORMAL_BLOCK);_free_dbg(crtBaseline,_NORMAL_BLOCK);
    });
    CHECK(known.live==0&&known.peak==128&&known.reallocOverlapPeak==192);
    CHECK(known.allocated==192&&known.freed==192&&known.baselineFreed==80&&known.allocations==2&&known.reallocations==1);
    // More successful request IDs than slots, followed by an untracked NORMAL baseline free.
    // This tests the recorder through actual allocation events rather than its own table API.
    constexpr auto count=rows.size()+1024;baseline=_malloc_dbg(80,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(baseline);if(!baseline)return;
    const auto reused=observe("recorder-request-reuse",[&]{
        for(std::size_t i=0;i<count;++i){void* block=_malloc_dbg(1,_NORMAL_BLOCK,__FILE__,__LINE__);CHECK(block);if(!block)break;_free_dbg(block,_NORMAL_BLOCK);}
        _free_dbg(baseline,_NORMAL_BLOCK);
    });
    CHECK(reused.valid&&reused.live==0&&reused.peak==1&&reused.allocations==count);
    CHECK(reused.allocated==count&&reused.freed==count&&reused.baselineFreed==80);
}
RackDesign maximum(bool portable) {
    auto d=good();d.name=juce::String::repeatedString("\\\"\n\t",16);d.intent=juce::String::repeatedString(d.name,8);d.revision=UINT64_MAX;
    d.panel.sections.resize(4,d.name);d.units[0].defaults.clear();const auto registry=parameterRegistry();
    for(const auto& p:registry)if(p.id.startsWith("synth.1."))d.units[0].defaults.push_back({0,p.id.substring(8),0});
    for(std::uint8_t id=1;id<=8;++id){RackUnit u{id,static_cast<NativeModule>(2+(id-1)%4),{}};
        for(const auto& p:registry.first(dynamicEqParameterStart))if(p.id.startsWith("effect.1."))u.defaults.push_back({id,p.id.substring(9),0});d.units.push_back(std::move(u));}
    d.panel.controls.clear();for(std::uint8_t id=1;id<=8;++id)d.panel.controls.push_back({d.name,id,static_cast<std::uint8_t>((id-1)%4),"bypassed",FocusWidget::toggle,FocusResponse::linear,0,1});
    if(portable)for(auto& u:d.units)std::erase_if(u.defaults,[&](const auto& p){
        if(u.module==NativeModule::analog)return p.key.startsWith("wavetable.");
        const auto prefix=u.module==NativeModule::drive?"drive.":u.module==NativeModule::delay?"delay.":u.module==NativeModule::reverb?"reverb.":"compressor.";
        return p.key!="bypassed"&&!p.key.startsWith(prefix);});
    CHECK(validateRackDesign(d,portable?RackValidationMode::portableDesign:RackValidationMode::instanceRecall));return d;
}
juce::Uuid identity(std::size_t index){std::array<juce::uint8,16> bytes{};bytes[0]=0xb1;bytes[14]=static_cast<juce::uint8>(index>>8);bytes[15]=static_cast<juce::uint8>(index+1);return juce::Uuid(bytes.data());}
struct Source { juce::MemoryBlock bytes;RackValidationMode mode=RackValidationMode::portableDesign; };
struct Entry { RackDesign definition;juce::String hash; };
std::size_t structural(const RackDesign& d) {
    std::size_t total=sizeof(RackDesign)+d.units.capacity()*sizeof(RackUnit)+d.panel.sections.capacity()*sizeof(juce::String)+d.panel.controls.capacity()*sizeof(FocusedControl);
    for(const auto& u:d.units)total+=u.defaults.capacity()*sizeof(LocalParameter);return total;
}
void denial_diagnostic() {
    begin("catalog_localized_recoverable_denial_diagnostic");
    // One warm native definition only; no 30/61 inventory, malformed trees or overlaps.
    const auto native=maximum(false);juce::MemoryBlock bytes;RackDesign warm;
    CHECK(encodeRackDesign(native,bytes,RackValidationMode::instanceRecall));
    CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),warm,RackValidationMode::instanceRecall));CHECK(warm==native);
    auto sentinel=good(),before=sentinel;std::vector<RackDesign> unchanged{good()};bool denied=false;
    std::cout<<"CATALOG_DENIAL_DIAGNOSTIC_CONTEXT iterator_debug_level="<<_ITERATOR_DEBUG_LEVEL
             <<" vector_default_noexcept="<<noexcept(std::vector<RackDesign>{})
             <<" new_handler_present="<<(std::get_new_handler()!=nullptr)
             <<" warn_mode="<<_CrtSetReportMode(_CRT_WARN,_CRTDBG_REPORT_MODE)
             <<" error_mode="<<_CrtSetReportMode(_CRT_ERROR,_CRTDBG_REPORT_MODE)
             <<" assert_mode="<<_CrtSetReportMode(_CRT_ASSERT,_CRTDBG_REPORT_MODE)<<std::endl;
    CHECK(_ITERATOR_DEBUG_LEVEL==2);
    denialTraceHandle=GetStdHandle(STD_OUTPUT_HANDLE);denialTracing=true;
    const auto oldReport=_CrtSetReportHook(denialReport);previousDenialReport=oldReport;
    try {
        // Move only setup out of denial. The intended reserve and rollback stay identical.
        denialMark("DENIAL_TRACE vector-constructor-before\n");std::vector<RackDesign> pending;
        denialMark("DENIAL_TRACE vector-constructor-after\n");
        const auto first=observe("first-vector-allocation-denied",[&]{try{
            denialMark("DENIAL_TRACE reserve-before\n");pending.reserve(61);
            denialMark("DENIAL_TRACE reserve-after\n");unchanged=std::move(pending);
        }catch(const std::bad_alloc&){denialMark("DENIAL_TRACE vector-bad-alloc-catch\n");denied=true;}},1);
        const auto vectorAttempts=allocationIndex;
        CHECK(denied&&first.denials==1&&unchanged.size()==1&&unchanged[0]==before);CHECK(vectorAttempts==1);
        // Allow JsonCheck's noexcept numeric proxy; reject the first throwing tree proxy.
        bool decodeAccepted=false,decodeDenied=false;
        const auto failure=observe("first-recoverable-decode-allocation-denied",[&]{try{
            denialMark("DENIAL_TRACE decode-before\n");decodeAccepted=decodeRackDesign(bytes.getData(),bytes.getSize(),sentinel,RackValidationMode::instanceRecall);
            denialMark("DENIAL_TRACE decode-after\n");
        }catch(const std::bad_alloc&){denialMark("DENIAL_TRACE decode-bad-alloc-catch\n");decodeDenied=true;}},2);
        CHECK(failure.denials==1&&!decodeAccepted&&sentinel==before);
        CHECK(allocationIndex==2&&failure.allocations==1&&failure.live==0&&decodeDenied);
        std::cout<<"DENIAL_RECOVERABLE_RESULT vector_attempts="<<vectorAttempts<<" decode_attempts="<<allocationIndex
                 <<" decode_allowed="<<failure.allocations<<" decode_denials="<<failure.denials
                 <<" decode_caught="<<decodeDenied<<" sentinel_unchanged="<<(sentinel==before)<<std::endl;
    }catch(...){_CrtSetReportHook(oldReport);denialTracing=false;throw;}
    _CrtSetReportHook(oldReport);denialTracing=false;
    std::cout<<"CATALOG_DENIAL_DIAGNOSTIC_COMPLETE limits=UNADOPTED catalog_product=ABSENT\n";
}
void run(bool suffixOnly=false) {
    begin(suffixOnly?"catalog_unobserved_calibration_suffix":"catalog_existing_codec_resource_calibration");
    if(!suffixOnly)recorder_self_checks();
    const auto outputText=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK11_CATALOG_CALIBRATION_OUTPUT",{});
    juce::File output;
    if(outputText.isNotEmpty()){output=juce::File(outputText);CHECK(!output.exists());if(output.exists())return;CHECK(output.createDirectory().wasOk());if(!output.isDirectory())return;}
    const auto native=maximum(false),portable=maximum(true);juce::MemoryBlock nativeBytes,portableBytes;
    CHECK(encodeRackDesign(native,nativeBytes,RackValidationMode::instanceRecall));CHECK(encodeRackDesign(portable,portableBytes));
    CHECK(nativeBytes.getSize()<=65536&&portableBytes.getSize()<=65536);
    if(nativeBytes.getSize()==0||portableBytes.getSize()==0)return;
    RackDesign warm;CHECK(decodeRackDesign(nativeBytes.getData(),nativeBytes.getSize(),warm,RackValidationMode::instanceRecall));
    CHECK(decodeRackDesign(portableBytes.getData(),portableBytes.getSize(),warm));
    auto factories=airack::test::frozenFactoryRackDesigns();CHECK(factories.size()==21);factories.resize(19); // Retain the historical61-source calibration workload.
    for(const auto& d:factories){juce::MemoryBlock bytes;CHECK(encodeRackDesign(d,bytes,RackValidationMode::instanceRecall));CHECK(decodeRackDesign(bytes.getData(),bytes.getSize(),warm,RackValidationMode::instanceRecall));}
    std::cout<<"CATALOG_ABI pointer="<<sizeof(void*)<<" string="<<sizeof(juce::String)<<" var="<<sizeof(juce::var)
             <<" design="<<sizeof(RackDesign)<<" unit="<<sizeof(RackUnit)<<" parameter="<<sizeof(LocalParameter)
             <<" control="<<sizeof(FocusedControl)<<" entry_model="<<sizeof(Entry)<<" memory_block="<<sizeof(juce::MemoryBlock)
             <<" source_model="<<sizeof(Source)<<" native_definition="<<nativeBytes.getSize()<<" portable_definition="<<portableBytes.getSize()<<'\n';
    if(!suffixOnly){
    juce::MemoryBlock encoded;observe("max-native-encode",[&]{CHECK(encodeRackDesign(native,encoded,RackValidationMode::instanceRecall));});CHECK(encoded==nativeBytes);
    RackDesign decoded;observe("max-native-decode",[&]{CHECK(decodeRackDesign(nativeBytes.getData(),nativeBytes.getSize(),decoded,RackValidationMode::instanceRecall));});CHECK(decoded==native);
    std::cout<<"CATALOG_STRUCTURAL units_size="<<decoded.units.size()<<" units_capacity="<<decoded.units.capacity()
             <<" sections_capacity="<<decoded.panel.sections.capacity()<<" controls_capacity="<<decoded.panel.controls.capacity()<<" structural_bytes="<<structural(decoded)<<'\n';
    for(const auto& u:decoded.units)std::cout<<"CATALOG_UNIT id="<<int(u.id)<<" size="<<u.defaults.size()<<" capacity="<<u.defaults.capacity()<<'\n';
    auto oversized=good();oversized.name.preallocateBytes(1048576);oversized.intent.preallocateBytes(1048576);oversized.units.reserve(8192);oversized.units[0].defaults.reserve(8192);oversized.panel.sections.reserve(8192);oversized.panel.controls.reserve(8192);
    const auto callerCapacity=oversized.units.capacity();juce::MemoryBlock small;CHECK(encodeRackDesign(oversized,small));
    RackDesign normalized;observe("oversized-caller-canonical-decode",[&]{CHECK(decodeRackDesign(small.getData(),small.getSize(),normalized));});
    CHECK(normalized==oversized&&normalized.units.capacity()<callerCapacity&&oversized.units.capacity()==callerCapacity);
    }
    std::vector<Source> mixed,stressSources;std::size_t canonicalTotal=0;std::size_t escapedTotal=0,stressTotal=0;
    for(std::size_t i=0;i<61;++i){auto d=i<factories.size()?factories[i]:i<29?native:portable;if(i>=factories.size())d.id=identity(i);
        Source s;s.mode=i<29?RackValidationMode::instanceRecall:RackValidationMode::portableDesign;CHECK(encodeRackDesign(d,s.bytes,s.mode));
        canonicalTotal+=s.bytes.getSize();const auto quoted=juce::JSON::toString(juce::var(juce::String::fromUTF8(static_cast<const char*>(s.bytes.getData()),static_cast<int>(s.bytes.getSize()))),true);
        escapedTotal+=static_cast<std::size_t>(quoted.getNumBytesAsUTF8());mixed.push_back(std::move(s));
        auto heavy=native;heavy.id=identity(i+100);Source t;t.mode=RackValidationMode::instanceRecall;CHECK(encodeRackDesign(heavy,t.bytes,t.mode));stressTotal+=t.bytes.getSize();stressSources.push_back(std::move(t));}
    CHECK(mixed.size()==61&&canonicalTotal<=3997696);std::cout<<"CATALOG_BYTES shape=mixed61 canonical="<<canonicalTotal<<" quoted_canonical="<<escapedTotal<<" source_fence_candidate=3997696 descriptor_claim=NONE\n";
    CHECK(stressSources.size()==61&&stressTotal<=3997696);std::cout<<"CATALOG_BYTES shape=max-retained61 canonical="<<stressTotal<<" source_fence_candidate=3997696 descriptor_claim=NONE\n";
    if(!suffixOnly){
    auto exercise=[&](std::size_t count,bool stress){
        const auto& sources=stress?stressSources:mixed;std::vector<Entry> oldEntries;
        const auto oldResult=observe(stress?"old-max-retained61":count==30?"old-mixed30":"old-mixed61",[&]{oldEntries.reserve(count);
            for(std::size_t i=0;i<count;++i){const auto& s=sources[i];Entry e;CHECK(decodeRackDesign(s.bytes.getData(),s.bytes.getSize(),e.definition,s.mode));e.hash=juce::SHA256(s.bytes).toHexString();oldEntries.push_back(std::move(e));}});
        std::vector<Entry> replacement;std::vector<RackDesign> staged;std::vector<Entry> page;
        const auto result=observe(stress?"overlap-max-retained61":count==30?"overlap-mixed30":"overlap-mixed61",[&]{
            replacement.reserve(count);
            for(std::size_t i=0;i<count;++i){const auto& s=sources[i];Entry e;
                CHECK(decodeRackDesign(s.bytes.getData(),s.bytes.getSize(),e.definition,s.mode));e.hash=juce::SHA256(s.bytes).toHexString();replacement.push_back(std::move(e));}
            page.reserve(1);page.push_back(replacement.back());staged.reserve(12);for(std::size_t i=0;i<12;++i)staged.push_back(replacement[i].definition);
            // One serial codec scratch phase overlaps the retained replacement/page/staging.
            juce::MemoryBlock scratch;CHECK(encodeRackDesign(replacement.back().definition,scratch,sources[count-1].mode));
        });
        CHECK(replacement.size()==count&&page.size()==1&&staged.size()==12&&oldEntries.size()==count);
        std::size_t structuralBytes=oldEntries.capacity()*sizeof(Entry)+replacement.capacity()*sizeof(Entry)+page.capacity()*sizeof(Entry)+staged.capacity()*sizeof(RackDesign);
        for(const auto& e:oldEntries)structuralBytes+=structural(e.definition)-sizeof(RackDesign);for(const auto& e:replacement)structuralBytes+=structural(e.definition)-sizeof(RackDesign);
        for(const auto& e:page)structuralBytes+=structural(e.definition)-sizeof(RackDesign);for(const auto& d:staged)structuralBytes+=structural(d)-sizeof(RackDesign);
        std::cout<<"CATALOG_OVERLAP entries="<<count<<" stress="<<stress<<" structural_bytes="<<structuralBytes<<" new_peak_requested="<<result.peak<<" old_retained_requested="<<oldResult.live<<" combined_peak_requested="<<oldResult.live+result.peak<<" measured_backing=ACTUAL_SHARING future_charge_policy=NO_SHARING structural_excludes_string_backing=1\n";
    };
    exercise(30,false);exercise(61,false);exercise(61,true);
    }
    auto sentinel=good(),before=sentinel;
    if(!suffixOnly){
    std::string padded(static_cast<const char*>(nativeBytes.getData()),nativeBytes.getSize());padded.resize(65536,' ');
    RackDesign paddedOutput;observe("padded65536-decode",[&]{CHECK(decodeRackDesign(padded.data(),padded.size(),paddedOutput,RackValidationMode::instanceRecall));});CHECK(paddedOutput==native);
    padded.push_back(' ');const auto tooLarge=observe("input65537-reject",[&]{CHECK(!decodeRackDesign(padded.data(),padded.size(),sentinel,RackValidationMode::instanceRecall));});CHECK(sentinel==before&&tooLarge.allocations==0);
    std::string numbers="[0";while(numbers.size()+3<=65536)numbers+=",0";numbers+=']';numbers.resize(65536,' ');
    observe("malformed-number-tree65536",[&]{CHECK(!decodeRackDesign(numbers.data(),numbers.size(),sentinel,RackValidationMode::instanceRecall));});CHECK(sentinel==before);
    }
    auto late=wire();juce::Array<juce::var> lateUnits;
    for(int unit=0;unit<9;++unit){juce::Array<juce::var> defaults;for(int p=0;p<140;++p){auto* v=new juce::DynamicObject;v->setProperty("unit",unit);v->setProperty("key","x");v->setProperty("normalized",0);defaults.add(juce::var(v));}
        auto* u=new juce::DynamicObject;u->setProperty("id",unit);u->setProperty("module","analog");u->setProperty("algorithm_revision",1);u->setProperty("defaults",defaults);lateUnits.add(juce::var(u));}
    late.getDynamicObject()->setProperty("units",lateUnits);const auto lateText=json(late);CHECK(lateText.size()<=65536);
    if(!suffixOnly){observe("malformed-late1260-defaults",[&]{CHECK(!decodeRackDesign(lateText.data(),lateText.size(),sentinel,RackValidationMode::instanceRecall));});CHECK(sentinel==before);}
    std::string keys="{";for(int i=0;;++i){const auto item=(i?",":"")+std::string("\"calibration_unknown_")+std::to_string(i)+"\":0";if(keys.size()+item.size()+1>65536)break;keys+=item;}keys+='}';keys.resize(65536,' ');
    if(!suffixOnly){observe("malformed-unique-property65536",[&]{CHECK(!decodeRackDesign(keys.data(),keys.size(),sentinel,RackValidationMode::instanceRecall));});CHECK(sentinel==before);}
    if(!suffixOnly){
    std::vector<RackDesign> unchanged{good()},pending;bool denied=false;
    // Construct the Debug noexcept proxy before rejecting the throwing reserve.
    const auto first=observe("first-vector-allocation-denied",[&]{try{pending.reserve(61);unchanged=std::move(pending);}catch(const std::bad_alloc&){denied=true;}},1);
    CHECK(denied&&first.denials==1&&unchanged.size()==1&&unchanged[0]==before);CHECK(allocationIndex==1);
    // The pinned Debug JsonCheck vector proxy is setup allocation1. The next
    // object set/tree proxy is throwing: this is first recoverable, not first overall OOM.
    bool decodeAccepted=false,decodeDenied=false;const auto failure=observe("first-recoverable-decode-allocation-denied",[&]{try{decodeAccepted=decodeRackDesign(nativeBytes.getData(),nativeBytes.getSize(),sentinel,RackValidationMode::instanceRecall);}catch(const std::bad_alloc&){decodeDenied=true;}},2);
    CHECK(failure.denials==1&&!decodeAccepted&&sentinel==before);
    CHECK(_ITERATOR_DEBUG_LEVEL==2&&allocationIndex==2&&failure.allocations==1&&failure.live==0&&decodeDenied);
    }
    std::size_t sum=0;auto add=[](std::size_t& value,std::size_t amount){if(amount>std::numeric_limits<std::size_t>::max()-value)return false;value+=amount;return true;};
    CHECK(add(sum,3997696)&&sum==3997696);CHECK(!add(sum,std::numeric_limits<std::size_t>::max())&&sum==3997696);
    if(output.isDirectory()){
        CHECK(output.getChildFile("max-native.rackdesign").replaceWithData(nativeBytes.getData(),nativeBytes.getSize()));CHECK(output.getChildFile("max-portable.rackdesign").replaceWithData(portableBytes.getData(),portableBytes.getSize()));
        for(std::size_t i=0;i<mixed.size();++i){CHECK(output.getChildFile("source-"+juce::String(static_cast<int>(i))+".rackdesign").replaceWithData(mixed[i].bytes.getData(),mixed[i].bytes.getSize()));CHECK(output.getChildFile("stress-"+juce::String(static_cast<int>(i))+".rackdesign").replaceWithData(stressSources[i].bytes.getData(),stressSources[i].bytes.getSize()));}
        CHECK(output.getChildFile("malformed-late1260.json").replaceWithData(lateText.data(),lateText.size()));CHECK(output.getChildFile("malformed-unique-property65536.json").replaceWithData(keys.data(),keys.size()));
        RackDesign fromFile;CHECK(loadRackDesign(output.getChildFile("source-29.rackdesign"),fromFile));
        observe("registered32-existing-load",[&]{for(std::size_t i=29;i<mixed.size();++i)CHECK(loadRackDesign(output.getChildFile("source-"+juce::String(static_cast<int>(i))+".rackdesign"),fromFile));});
        std::string portablePadded(static_cast<const char*>(portableBytes.getData()),portableBytes.getSize());portablePadded.resize(65536,' ');const auto paddedFile=output.getChildFile("padded65536.rackdesign");CHECK(paddedFile.replaceWithData(portablePadded.data(),portablePadded.size()));
        observe("file-input65536-existing-load",[&]{CHECK(loadRackDesign(paddedFile,fromFile));});CHECK(fromFile==portable);
        portablePadded.push_back(' ');const auto oversizedFile=output.getChildFile("input65537.rackdesign");CHECK(oversizedFile.replaceWithData(portablePadded.data(),portablePadded.size()));const auto previous=fromFile;
        observe("file-input65537-existing-reject",[&]{CHECK(!loadRackDesign(oversizedFile,fromFile));});CHECK(fromFile==previous);
    }
    if(suffixOnly)std::cout<<"CATALOG_CALIBRATION_SUFFIX_ONLY prior_resources=C16 prior_denials=E whole_calibration_pass=UNCLAIMED limits=UNADOPTED catalog_product=ABSENT\n";
    else std::cout<<"CATALOG_CALIBRATION domain=quiescent-warmed-current-thread-debug-CRT-payload global_pool_universal_bound=UNCLAIMED descriptor_budget=UNADOPTED catalog_product=ABSENT\n";
}
}
#endif
bool catalog_denial_diagnostic_if_requested() {
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto selected=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK11_DENIAL_DIAGNOSTIC",{});
    if(selected.isNotEmpty()){CHECK(selected=="1");if(selected=="1")catalogCalibration::denial_diagnostic();return true;}
#endif
    return false;
}
void catalog_existing_codec_resource_calibration() {
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto suffix=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK11_CALIBRATION_SUFFIX",{});
    const auto whole=juce::SystemStats::getEnvironmentVariable("AIRACK_TASK11_RESOURCE_CALIBRATION",{});
    if(suffix.isEmpty()&&whole.isEmpty())return; // Historical C/E/F measurements stay opt-in; ordinary main owns product checks.
    if(suffix.isNotEmpty())CHECK(suffix=="1");
    if(whole.isNotEmpty())CHECK(whole=="1");
    catalogCalibration::run(suffix=="1");
#endif
}

namespace {
void catalog_all_pages_custom_identity() {
    begin(__func__);auto factories=airack::test::frozenLibraryRackDesigns();const auto historicalTile=airack::test::frozenTileArrayRackDesign();
    CHECK(factories.size()==28&&std::count(factories.begin(),factories.end(),historicalTile)==1);
    std::erase(factories,historicalTile);std::erase_if(factories,[](const auto& d){return d.units[0].module==NativeModule::saturation;});
    CHECK(factories.size()==25);CHECK(maximumRackCatalogEntries==72);
    RackCatalogCaptureLimits historicalLimits;historicalLimits.maximumEntries=historicalLimits.maximumSources=67;
    std::vector<RackDesign> embedded;
    for(int n=0;n<12;++n){auto d=good();d.id=juce::Uuid();d.name="Embedded "+juce::String(n);d.revision=4294967297ULL+std::uint64_t(n);embedded.push_back(std::move(d));}
    auto inventory=embedded;
    for(int n=0;n<30;++n){auto d=good();d.id=juce::Uuid();d.name="Custom \""+juce::String(n)+"\\";d.intent="UTF8 café \"quote\" \\ path";inventory.push_back(std::move(d));}
    RackCatalogSnapshot catalog;RackCatalogStatus status=RackCatalogStatus::unavailable;
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto capacityHeap=catalogCalibration::observe("space-full67-owned-capture",[&]{CHECK(captureRackCatalog(factories,inventory,historicalLimits,catalog,&status));});
    CHECK(capacityHeap.valid&&capacityHeap.peak<=41943040&&capacityHeap.live<=41943040);
#else
    CHECK(captureRackCatalog(factories,inventory,historicalLimits,catalog,&status));
#endif
    CHECK(status==RackCatalogStatus::ready&&catalog.entries.size()==67&&!catalog.id.isNull());
    if(catalog.entries.size()!=67)return;
    {auto extra=inventory;auto d=good();d.id=juce::Uuid();d.name="Embedded 12";extra.push_back(std::move(d));RackCatalogSnapshot over;
     CHECK(!captureRackCatalog(factories,extra,historicalLimits,over,&status)&&status==RackCatalogStatus::countLimit);}
    RackDesign resolved;const auto last=catalog.entries.back();
    CHECK(resolveCatalogDesign(catalog,last.definition.id,last.definition.revision,last.contentSha256,resolved));CHECK(resolved==last.definition);
    const auto sentinel=resolved;
    CHECK(!resolveCatalogDesign(catalog,last.definition.id,last.definition.revision+1,last.contentSha256,resolved));CHECK(resolved==sentinel);
    CHECK(!resolveCatalogDesign(catalog,last.definition.id,last.definition.revision,juce::String::repeatedString("0",64),resolved));CHECK(resolved==sentinel);
    auto tampered=catalog;tampered.entries.back().definition.name="Altered";
    CHECK(!resolveCatalogDesign(tampered,last.definition.id,last.definition.revision,last.contentSha256,resolved));CHECK(resolved==sentinel);
    std::size_t cursor=0,count=0;int pages=0;while(cursor<catalog.entries.size()){
        RackCatalogPage page;CHECK(catalogPage(catalog,{},cursor,8192,page,&status));if(status!=RackCatalogStatus::ready)break;
        CHECK(!page.entries.empty()&&page.next>cursor&&page.snapshot==catalog.id);std::size_t escaped=0;
        CHECK(rackCatalogEscapedSize(rackCatalogPageDescriptor(page),8192,escaped));CHECK(escaped<=8192);
        for(const auto& entry:page.entries){CHECK(entry==catalog.entries[count]);++count;}cursor=page.next;++pages;
        CHECK(page.complete==(cursor==catalog.entries.size()));
    }CHECK(count==67&&pages>1);
    RackCatalogPage page;CHECK(catalogPage(catalog,{"CUSTOM",RackRole::synth},0,65536,page));CHECK(page.complete&&page.entries.size()==30);
    CHECK(catalogPage(catalog,{"does not exist",{}},0,65536,page));CHECK(page.complete&&page.entries.empty()&&page.next==67);
    std::size_t exact=0;CHECK(rackCatalogEscapedSize(rackCatalogPageDescriptor(page),65536,exact));
    CHECK(catalogPage(catalog,{"does not exist",{}},67,exact,page));CHECK(page.complete&&page.entries.empty());
    const auto oldPage=page;CHECK(!catalogPage(catalog,{},68,65536,page,&status));CHECK(status==RackCatalogStatus::invalidCursor&&page.snapshot==oldPage.snapshot&&page.entries==oldPage.entries);
    CHECK(!catalogPage(catalog,{},0,1,page,&status));CHECK(status==RackCatalogStatus::singleEntryTooLarge&&page.entries==oldPage.entries);
    CHECK(!catalogPage(catalog,{juce::String::repeatedString("x",513),{}},0,65536,page,&status));CHECK(status==RackCatalogStatus::invalidQuery);
    auto duplicate=embedded;duplicate.push_back(embedded[0]);RackCatalogSnapshot unique;
    CHECK(captureRackCatalog({},duplicate, {},unique));CHECK(unique.entries.size()==12);
    duplicate.back().name="Conflict";const auto previous=unique;
    CHECK(!captureRackCatalog({},duplicate, {},unique,&status));CHECK(status==RackCatalogStatus::identityConflict&&unique.id==previous.id&&unique.entries==previous.entries);
}
void catalog_fresh_ownership_and_resource_fences() {
    begin(__func__);auto d=good();d.name.preallocateBytes(1024*1024);d.intent.preallocateBytes(1024*1024);d.units.reserve(1024);d.panel.controls.reserve(1024);d.units[0].defaults.reserve(1024);
    std::array<RackDesign,1> sources{d};RackCatalogSnapshot catalog;CHECK(captureRackCatalog(sources,{}, {},catalog));CHECK(catalog.entries.size()==1);
    if(catalog.entries.size()!=1)return;const auto& owned=catalog.entries[0].definition;
    CHECK(owned==d&&owned.name.toRawUTF8()!=sources[0].name.toRawUTF8()&&owned.intent.toRawUTF8()!=sources[0].intent.toRawUTF8());
    CHECK(owned.units.capacity()==owned.units.size()&&owned.panel.controls.capacity()==owned.panel.controls.size()&&owned.units[0].defaults.capacity()==owned.units[0].defaults.size());
    CHECK(sizeof(juce::String)<=8&&sizeof(juce::var)<=16&&sizeof(RackCatalogEntry)<=304&&sizeof(RackCatalogSnapshot)<=512&&sizeof(RackCatalogPage)<=512);
    const auto before=catalog;RackCatalogStatus status;RackCatalogCaptureLimits limits;juce::MemoryBlock canonical;
    CHECK(encodeRackDesign(d,canonical,RackValidationMode::instanceRecall));std::size_t descriptor=0;
    CHECK(rackCatalogEscapedSize(rackCatalogDescriptor(catalog.entries[0]),3997696,descriptor));
    limits.maximumDefinitionBytes=canonical.getSize();limits.maximumSourceBytes=canonical.getSize();limits.maximumDescriptorBytes=descriptor;
    CHECK(captureRackCatalog(sources,{},limits,catalog));
    auto rejects=[&](RackCatalogCaptureLimits fence,RackCatalogStatus expected){CHECK(!captureRackCatalog(sources,{},fence,catalog,&status));CHECK(status==expected&&catalog.id==before.id&&catalog.entries==before.entries);};
    auto fence=limits;--fence.maximumDefinitionBytes;rejects(fence,RackCatalogStatus::byteLimit);
    fence=limits;--fence.maximumSourceBytes;rejects(fence,RackCatalogStatus::byteLimit);
    fence=limits;--fence.maximumDescriptorBytes;rejects(fence,RackCatalogStatus::byteLimit);
    fence=limits;fence.maximumEntries=0;rejects(fence,RackCatalogStatus::countLimit);
    fence=limits;fence.maximumSources=0;rejects(fence,RackCatalogStatus::countLimit);
    fence=limits;fence.maximumPreparedBytes=0;rejects(fence,RackCatalogStatus::preparedLimit);
    fence=limits;fence.maximumSourceBytes=std::numeric_limits<std::size_t>::max();rejects(fence,RackCatalogStatus::byteLimit);
    std::vector<RackDesign> repeated(maximumRackCatalogEntries,d);RackCatalogSnapshot dedup;
    CHECK(captureRackCatalog(repeated,{},{},dedup));CHECK(dedup.entries.size()==1);
    repeated.push_back(d);CHECK(!captureRackCatalog(repeated,{},{},catalog,&status));CHECK(status==RackCatalogStatus::countLimit&&catalog.entries==before.entries);
#if defined(_MSC_VER) && defined(_DEBUG)
    using namespace catalogCalibration;RackCatalogSnapshot failed=before;
    // Two Debug noexcept vector proxies are setup; reject the following throwing reserve.
    const auto observed=observe("product-capture-first-recoverable-reserve",[&]{CHECK(!captureRackCatalog(sources,{},{},failed,&status));},3);
    CHECK(observed.valid&&observed.denials==1&&status==RackCatalogStatus::allocationFailure&&failed.id==before.id&&failed.entries==before.entries);
    std::cout<<"CATALOG_PRODUCT_CAPTURE_DENIAL attempts="<<allocationIndex<<" allowed="<<observed.allocations<<" denied="<<observed.denials<<" live="<<observed.live<<" prior_unchanged=1 first_overall_OOM=UNPROVEN\n";
    const auto preflight=observe("product-prepared-preflight",[&]{auto limited=limits;limited.maximumPreparedBytes=0;CHECK(!captureRackCatalog(sources,{},limited,failed,&status));});
    CHECK(preflight.allocations==0&&preflight.freed==0&&failed.entries==before.entries&&status==RackCatalogStatus::preparedLimit);
#endif
}
// The studio support spec's names for every module and effect kind (decision 1).
void display_names(){
    begin("display_names");
    const char* const names[]={"Analog","Wavetable","Drive","Delay","Reverb","Compressor","EQ","Filter","Chorus","Phaser","Flanger","Stereo","Dimension","Frequency shift","Convolution","Frequency split","Mid/side split","Dynamic EQ","Space Reverb","Era Reverb","Drum","Saturation"};
    for(int m=0;m<22;++m)CHECK(moduleDisplayName(NativeModule(m))==names[m]);
    for(int k=0;k<18;++k)CHECK(effectDisplayName(EffectKind(k))==names[k+2]);
    CHECK(moduleDisplayName(NativeModule(22))=="Unknown"&&effectDisplayName(EffectKind(19))=="Unknown"&&effectDisplayName(EffectKind::saturation)=="Saturation");
}
void library_designs(){
    begin("library_designs");
    const auto library=airack::test::frozenLibraryRackDesigns();const auto examples=airack::test::frozenExampleRackDesigns();const auto factories=airack::test::frozenFactoryRackDesigns();
    CHECK(library.size()==28&&examples.size()==6&&factories.size()==21);if(library.size()!=28||examples.size()!=6||factories.size()!=21)return;
    const auto tile=airack::test::frozenTileArrayRackDesign();CHECK(library.back()==tile);
    CHECK(std::count_if(library.begin(),library.end(),[&](const auto& d){return d.id==tile.id;})==1);
    for(std::size_t i=0;i<6;++i)CHECK(library[i]==examples[i]);
    for(std::size_t i=0;i<21;++i)CHECK(library[6+i]==factories[i]);
}
// Oxide Return (docs/design/racks/2026-10-09-studio-rack/oxide-return/rack-brief.json): ARDENTI's tape echo, one
// revision 2 delay whose recipe sets the five tape keys, with seven controls on its face.
void oxide_return_design(){
    begin("oxide_return_design");
    const auto examples=airack::test::frozenExampleRackDesigns();CHECK(examples.size()==6);if(examples.size()<4)return;
    const auto& d=examples[3];
    CHECK(d.id==juce::Uuid("330cb53b-cd9b-443c-94d8-2700bc8b88f7")&&d.revision==1&&d.name=="Oxide Return"&&d.role==RackRole::fx);
    CHECK(d.intent=="A warm tape-style echo whose repeats saturate, darken and thin as they recirculate, with a gentle wow; time changes "
                    "glide like a tape transport. Shape the echo time, how long the repeats return, how much tape grit and wobble they "
                    "gather and the low and high cut on the repeats; the dry signal stays clean. Switch to host-synced note values, cross "
                    "the repeats between the sides, or speed the wobble up toward flutter when needed.");
    CHECK(validateRackDesign(d,RackValidationMode::portableDesign));
    CHECK(d.units.size()==1);if(d.units.size()!=1)return;
    const auto& u=d.units[0];CHECK(u.id==0&&u.module==NativeModule::delay&&u.algorithmRevision==2);
    const std::vector<std::pair<const char*,float>> recipe{{"bypassed",0.f},{"delay.synced",0.f},{"delay.seconds",.3f},{"delay.beats",.5f},
        {"delay.feedback",.38f},{"delay.stereo",.3f},{"delay.filter_hz",5000.f},{"delay.mix",.25f},{"delay.saturation",.3f},
        {"delay.low_cut_hz",100.f},{"delay.wobble",.2f},{"delay.wobble_rate_hz",.8f},{"delay.glide",1.f},{"delay.note",11.f}};
    // NOTE reads 1/8, as the face's knob did; RIGHT and ping-pong keep their defaults outside the recipe.
    CHECK(u.defaults.size()==recipe.size());
    for(std::size_t i=0;i<recipe.size()&&i<u.defaults.size();++i){
        const auto& v=u.defaults[i];const auto index=rackParameterIndex(NativeModule::delay,0,recipe[i].first);CHECK(index.has_value());if(!index)continue;
        const auto& p=parameterRegistry()[*index];const float physical=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertFrom0to1(v.normalized);
        CHECK(v.unit==0&&v.key==recipe[i].first&&std::abs(physical-recipe[i].second)<=1e-4f*std::max(1.f,std::abs(recipe[i].second)));
    }
    CHECK(d.panel.layout==PanelLayout::orderedFxStrip&&d.panel.sections==std::vector<juce::String>({"Echo","Tape","Tone","Output"}));
    const std::vector<FocusedControl> face{
        {"Time",0,0,"delay.seconds",FocusWidget::rotary,FocusResponse::logarithmic,.02f,2},
        {"Repeats",0,0,"delay.feedback",FocusWidget::rotary,FocusResponse::linear,0,.95f},
        {"Saturation",0,1,"delay.saturation",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Wobble",0,1,"delay.wobble",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Low cut",0,2,"delay.low_cut_hz",FocusWidget::rotary,FocusResponse::logarithmic,20,2000},
        {"High cut",0,2,"delay.filter_hz",FocusWidget::rotary,FocusResponse::logarithmic,1000,20000},
        {"Mix",0,3,"delay.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
    CHECK(d.panel.controls==face);
}
// Small Hours (docs/design/racks/2026-10-09-studio-rack/small-hours/rack-brief.json): NACHTFELD's dark vintage
// reverb, one Era Reverb unit whose recipe sets all fifteen keys, with eight controls on its face.
void small_hours_design(){
    begin("small_hours_design");
    const auto examples=airack::test::frozenExampleRackDesigns();CHECK(examples.size()==6);if(examples.size()<6)return;
    const auto& d=examples[4];
    CHECK(d.id==juce::Uuid("22d80511-2e4f-4a41-b1dd-203eabbcf32e")&&d.revision==1&&d.name=="Small Hours"&&d.role==RackRole::fx);
    CHECK(d.intent=="A dark, vintage-coloured reverb voiced for cinematic future garage: lush slow-building pad washes, pitch-steady "
                    "spaces for vocal chops and bells, tape-wobbled halls, dense plates for snares and claps, airy rooms, and gated or "
                    "reverse swells. Shape the voicing, decay, size and swell, the era's darkness, the wobble's depth and speed and a "
                    "little grit; low and high cut keep the tail clear of the sub and the hats.");
    CHECK(validateRackDesign(d,RackValidationMode::portableDesign));
    CHECK(d.units.size()==1);if(d.units.size()!=1)return;
    const auto& u=d.units[0];CHECK(u.id==0&&u.module==NativeModule::eraReverb&&u.algorithmRevision==1);
    const std::vector<std::pair<const char*,float>> recipe{{"bypassed",0.f},{"era_reverb.voicing",1.f},{"era_reverb.age",0.f},
        {"era_reverb.decay_seconds",8.f},{"era_reverb.size",.85f},{"era_reverb.attack",.6f},{"era_reverb.predelay_ms",25.f},
        {"era_reverb.wobble",.4f},{"era_reverb.wobble_rate_hz",.7f},{"era_reverb.grit",.15f},{"era_reverb.diffusion",1.f},
        {"era_reverb.bass_multiplier",1.2f},{"era_reverb.damping_hz",5000.f},{"era_reverb.low_cut_hz",150.f},{"era_reverb.high_cut_hz",9000.f},
        {"era_reverb.mix",.3f}};
    CHECK(u.defaults.size()==recipe.size());
    for(std::size_t i=0;i<recipe.size()&&i<u.defaults.size();++i){
        const auto& v=u.defaults[i];const auto index=rackParameterIndex(NativeModule::eraReverb,0,recipe[i].first);CHECK(index.has_value());if(!index)continue;
        const auto& p=parameterRegistry()[*index];const float physical=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertFrom0to1(v.normalized);
        CHECK(v.unit==0&&v.key==recipe[i].first&&std::abs(physical-recipe[i].second)<=1e-4f*std::max(1.f,std::abs(recipe[i].second)));
    }
    CHECK(d.panel.layout==PanelLayout::orderedFxStrip&&d.panel.sections==std::vector<juce::String>({"Space","Age","Blend"}));
    const std::vector<FocusedControl> face{
        {"Program",0,0,"era_reverb.voicing",FocusWidget::choice,FocusResponse::linear,0,5},
        {"Afterglow",0,0,"era_reverb.decay_seconds",FocusWidget::rotary,FocusResponse::logarithmic,.2f,70},
        {"Depth",0,0,"era_reverb.size",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Swell",0,0,"era_reverb.attack",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Age",0,1,"era_reverb.age",FocusWidget::choice,FocusResponse::linear,0,2},
        {"Wow",0,1,"era_reverb.wobble",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Grit",0,1,"era_reverb.grit",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Mix",0,2,"era_reverb.mix",FocusWidget::rotary,FocusResponse::linear,0,1}};
    CHECK(d.panel.controls==face);
    // The AI's view: the catalog row carries the unit's recipe keys with their registry units and ranges.
    const auto row=rackCatalogDescriptor({d,juce::String::repeatedString("a",64)});
    CHECK(row["name"].toString()=="Small Hours"&&row["intent"].toString()==d.intent&&row["role"].toString()=="fx");
    const auto* units=row["units"].getArray();CHECK(units&&units->size()==1);if(!units||units->size()!=1)return;
    CHECK(int((*units)[0]["module"])==int(NativeModule::eraReverb)&&int((*units)[0]["algorithm_revision"])==1);
    const auto* rows=(*units)[0]["parameters"].getArray();CHECK(rows&&rows->size()==int(recipe.size()));if(!rows||rows->size()!=int(recipe.size()))return;
    for(std::size_t i=0;i<recipe.size();++i){
        const auto& r=(*rows)[int(i)];const auto& p=parameterRegistry()[*rackParameterIndex(NativeModule::eraReverb,0,recipe[i].first)];
        CHECK(r[0].toString()==recipe[i].first&&r[1].toString()==juce::String(p.unit)&&double(r[2])==double(p.minimum)&&double(r[3])==double(p.maximum));
    }
}

// Night Dial (docs/design/racks/2026-10-09-studio-rack/night-dial/rack-brief.json): Elmstead Signal Works' hybrid
// voice, one revision 2 wavetable unit whose recipe sets the warmth keys, with eight controls on its face.
void night_dial_design(){
    begin("night_dial_design");
    const auto examples=airack::test::frozenExampleRackDesigns();CHECK(examples.size()==6);if(examples.size()<6)return;
    const auto& d=examples[5];
    CHECK(d.id==juce::Uuid("cdeafe3f-9c29-4c7c-9013-8284b505af10")&&d.revision==1&&d.name=="Night Dial"&&d.role==RackRole::synth);
    CHECK(d.intent=="A moving wavetable voice with analogue warmth for cinematic future garage: wide drifting pads, slow swells, "
                    "evolving textures, reese and sub bass, vowel choirs and soft plucks. Shape the table position and its sweep, "
                    "unison spread, drift, the filter (clean, ladder, classic or vowel) and the swell and tail. Space, tape and vinyl "
                    "come from FX racks."&&d.intent.length()==349);
    CHECK(validateRackDesign(d,RackValidationMode::portableDesign));
    CHECK(d.units.size()==1);if(d.units.size()!=1)return;
    const auto& u=d.units[0];CHECK(u.id==0&&u.module==NativeModule::wavetable&&u.algorithmRevision==2);
    const std::vector<std::pair<const char*,float>> recipe{{"enabled",1.f},{"engine",1.f},
        {"wavetable.osc.1.table",0.f},{"wavetable.osc.1.position",.45f},{"wavetable.osc.1.level",.5f},{"wavetable.osc.1.unison",4.f},{"wavetable.osc.1.unison_cents",15.f},
        {"wavetable.osc.2.table",0.f},{"wavetable.osc.2.position",.1f},{"wavetable.osc.2.tune",-12.f},{"wavetable.osc.2.level",.25f},{"wavetable.osc.2.unison",0.f},
        {"wavetable.unison_width",.7f},{"wavetable.drift",.35f},{"wavetable.filter.type",2.f},{"wavetable.filter.mode",0.f},{"wavetable.filter.cutoff",1200.f},{"wavetable.filter.resonance",.15f},
        {"wavetable.amp.attack",1.5f},{"wavetable.amp.attack_curve",-.5f},{"wavetable.amp.sustain",.9f},{"wavetable.amp.release",3.f},
        {"lfo.1.shape",0.f},{"lfo.1.synced",0.f},{"lfo.1.rate_hz",.08f},
        {"modulation.1.enabled",1.f},{"modulation.1.source",3.f},{"modulation.1.target",11.f},{"modulation.1.bank",2.f},{"modulation.1.amount",.3f},
        {"modulation.2.enabled",1.f},{"modulation.2.source",5.f},{"modulation.2.target",14.f},{"modulation.2.bank",2.f},{"modulation.2.amount",.06f}};
    CHECK(u.defaults.size()==recipe.size());
    for(std::size_t i=0;i<recipe.size()&&i<u.defaults.size();++i){
        const auto& v=u.defaults[i];const auto index=rackParameterIndex(NativeModule::wavetable,0,recipe[i].first);CHECK(index.has_value());if(!index)continue;
        const auto& p=parameterRegistry()[*index];const float physical=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertFrom0to1(v.normalized);
        CHECK(v.unit==0&&v.key==recipe[i].first&&std::abs(physical-recipe[i].second)<=1e-4f*std::max(1.f,std::abs(recipe[i].second)));
    }
    CHECK(d.panel.layout==PanelLayout::instrumentSections&&d.panel.sections==std::vector<juce::String>({"Signal","Warmth","Contour"}));
    const std::vector<FocusedControl> face{
        {"Dial",0,0,"wavetable.osc.1.position",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Sweep",0,0,"modulation.1.amount",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Spread",0,0,"wavetable.osc.1.unison_cents",FocusWidget::rotary,FocusResponse::linear,0,50},
        {"Wander",0,1,"wavetable.drift",FocusWidget::rotary,FocusResponse::linear,0,1},
        {"Glow",0,1,"wavetable.filter.cutoff",FocusWidget::rotary,FocusResponse::logarithmic,100,12000},
        {"Ring",0,1,"wavetable.filter.resonance",FocusWidget::rotary,FocusResponse::linear,0,.9f},
        {"Swell",0,2,"wavetable.amp.attack",FocusWidget::rotary,FocusResponse::logarithmic,.005f,8},
        {"Tail",0,2,"wavetable.amp.release",FocusWidget::rotary,FocusResponse::logarithmic,.05f,10}};
    CHECK(d.panel.controls==face);
    // The AI's view: the catalog row carries the unit's recipe keys, the warmth keys among them, with their registry units and ranges.
    const auto row=rackCatalogDescriptor({d,juce::String::repeatedString("a",64)});
    CHECK(row["name"].toString()=="Night Dial"&&row["intent"].toString()==d.intent&&row["role"].toString()=="synth");
    const auto* units=row["units"].getArray();CHECK(units&&units->size()==1);if(!units||units->size()!=1)return;
    CHECK(int((*units)[0]["module"])==int(NativeModule::wavetable)&&int((*units)[0]["algorithm_revision"])==2);
    const auto* rows=(*units)[0]["parameters"].getArray();CHECK(rows&&rows->size()==int(recipe.size()));if(!rows||rows->size()!=int(recipe.size()))return;
    for(std::size_t i=0;i<recipe.size();++i){
        const auto& r=(*rows)[int(i)];const auto& p=parameterRegistry()[*rackParameterIndex(NativeModule::wavetable,0,recipe[i].first)];
        CHECK(r[0].toString()==recipe[i].first&&r[1].toString()==juce::String(p.unit)&&double(r[2])==double(p.minimum)&&double(r[3])==double(p.maximum));
    }
    for(const auto* warmth:{"wavetable.osc.1.unison","wavetable.osc.1.unison_cents","wavetable.unison_width","wavetable.drift","wavetable.filter.type","wavetable.amp.attack_curve"})
        CHECK(std::any_of(rows->begin(),rows->end(),[&](const juce::var& r){return r[0].toString()==warmth;}));
}
// Decision 5's outside list, for one parameter of the sound.
bool outsideAllowed(const SoundDocument& d, std::size_t i) {
    const juce::String id(parameterRegistry()[i].id);
    if (id == "output.render_quality") return true;
    const bool synth = id.startsWith("synth.");
    if (!synth && !id.startsWith("effect.")) return false;
    const auto rest = id.fromFirstOccurrenceOf(".", false, false);
    const auto bank = (std::uint8_t) (rest.getIntValue() - 1);
    const auto key = rest.fromFirstOccurrenceOf(".", false, false);
    const bool present = synth ? d.layout.synthUse[bank] == SlotUse::present : d.layout.effects[bank].use == SlotUse::present;
    const auto module = bankModule(d, synth ? RackRole::synth : RackRole::fx, bank);
    const bool legacyWarmth = module == NativeModule::wavetable && i >= wavetableWarmthParameterStart && d.compatibility.synth[bank][1] == legacyAlgorithmRevision;
    return !present || key == "engine" || key == "drum_mode" || legacyWarmth || !rackParameterActive(module, key);
}
void checkReachPartition(const SoundDocument& d, const juce::String& name) {
    const auto reach = userReach(d);
    std::bitset<parameterCount> expected;
    for (const auto i : houseParameters()) expected.set(i);
    for (std::uint8_t b = 0; b < 4; ++b)
        if (d.layout.synthUse[b] == SlotUse::present) {
            const auto module = bankModule(d, RackRole::synth, b);
            for (const auto i : unitReach(module, b, rackAlgorithmRevision(d.compatibility,module,b))) expected.set(i);
        }
    for (std::uint8_t b = 0; b < 8; ++b)
        if (d.layout.effects[b].use == SlotUse::present) for (const auto i : unitReach(bankModule(d, RackRole::fx, b), b)) expected.set(i);
    CHECK(reach == expected);
    int stray = 0;
    for (std::size_t i = 0; i < parameterCount; ++i)
        if (!reach[i] && !outsideAllowed(d, i) && ++stray <= 5) std::cerr << name << ": out of reach " << parameterRegistry()[i].id << '\n';
    CHECK(stray == 0);
}
// Decisions 2 and 5: the user reaches the house unit and every present bank's active controls.
void user_reach() {
    begin("user_reach");
    const auto house = houseParameters();
    CHECK(house.size() == 30 && std::adjacent_find(house.begin(), house.end(), std::greater_equal<>()) == house.end());
    for (std::uint8_t bank = 0; bank < 4; ++bank)
        for (const auto module : {NativeModule::analog, NativeModule::wavetable}) {
            const auto reach = unitReach(module, bank);
            CHECK(reach.size() == (module == NativeModule::wavetable ? 125u : 116u) && std::find(reach.begin(), reach.end(), 13 + 4 * std::size_t(bank)) == reach.end());
            CHECK(unitReach(module, bank, legacyAlgorithmRevision).size() == 116);
        }
    const std::size_t effectSizes[] {5, 16, 6, 7, 10, 6, 6, 6, 6, 5, 5, 4, 5, 9, 8};
    for (int k = 0; k < 15; ++k)
        for (std::uint8_t bank = 0; bank < 8; ++bank) CHECK(unitReach(NativeModule(k + 2), bank).size() == effectSizes[k]);
    checkReachPartition(SoundDocument {}, "default");
    for (const auto& design : airack::test::frozenLibraryRackDesigns()) {
        RackOperation op; op.design = design; op.newInstance = juce::Uuid();
        op.destination.role = design.role; op.destination.chain = masterChain;
        SoundDocument placed; RackDifference delta;
        const bool ready = makeRackCandidate(SoundDocument {}, op, SoundEditOrigin::manual, placed, delta) == RackCandidateStatus::ready;
        CHECK(ready); if (!ready) { std::cerr << design.name << " was not placed\n"; continue; }
        for (const auto& binding : placed.personalities->instances[0].bindings) {
            const auto module = std::find_if(design.units.begin(), design.units.end(), [&](const auto& u) { return u.id == binding.unit; })->module;
            CHECK(bankModule(placed, isSourceModule(module) ? RackRole::synth : RackRole::fx, binding.slot) == module);
        }
        checkReachPartition(placed, design.name);
    }
    SoundDocument all;
    for (int n = 0; n < 3; ++n) { RackEdit e; e.kind = RackEditKind::addSynth; CHECK(editRackLayout(all.layout, e, all.layout) == RackEditResult::applied); }
    for (int k = 0; k < 8; ++k) { RackEdit e; e.kind = RackEditKind::addEffect; e.effect = EffectKind(k); CHECK(editRackLayout(all.layout, e, all.layout) == RackEditResult::applied); }
    setPhysical(all, "synth.2.engine", 1); setPhysical(all, "synth.4.engine", 1);
    CHECK(bankModule(all, RackRole::synth, 1) == NativeModule::wavetable && bankModule(all, RackRole::synth, 2) == NativeModule::analog && bankModule(all, RackRole::synth, 3) == NativeModule::wavetable);
    checkReachPartition(all, "all banks");
}
// The studio feedback spec, decision 4: a new instance's sound has no synth bank, no effect and no rack, with the new
// instance's kernels. Copper Arc alone on synth 1, the start before it, stays copperArcSound().
void new_instance_sound() {
    begin("new_instance_sound");
    const auto empty = newInstanceSound();
    CHECK(validateSoundDocument(empty) && empty.compatibility == newInstanceKernelCompatibility());
    CHECK(!empty.personalities || empty.personalities->instances.empty());
    CHECK(empty.layout.synthCount == 0 && std::all_of(empty.layout.synthUse.begin(), empty.layout.synthUse.end(), [](auto u) { return u == SlotUse::neverUsed; }));
    CHECK(std::all_of(empty.layout.effects.begin(), empty.layout.effects.end(), [](const auto& e) { return e.use == SlotUse::neverUsed; }));
    CHECK(newInstanceSound() == empty);
    checkReachPartition(empty, "new instance");
    const auto d = copperArcSound();
    CHECK(validateSoundDocument(d) && d.compatibility == newInstanceKernelCompatibility());
    const auto copper = airack::test::frozenFactoryRackDesigns()[0];
    CHECK(copper.name == "Copper Arc" && d.personalities && d.personalities->instances.size() == 1);
    if (!d.personalities || d.personalities->instances.size() != 1) return;
    const auto& rack = d.personalities->instances[0];
    CHECK(rack.id == juce::Uuid("91387715-13d3-4c57-a479-12251842a4f1") && rack.design.id == copper.id && rack.design.revision == copper.revision);
    CHECK(rack.bindings == (std::vector<UnitBinding>{{0, 0}}));
    const SoundDocument blank;
    CHECK(d.layout.synthUse == blank.layout.synthUse && d.layout.synthOrder == blank.layout.synthOrder && d.layout.effects == blank.layout.effects);
    for (const auto& p : copper.units[0].defaults) CHECK(d.values[*rackParameterIndex(NativeModule::analog, 0, p.key)] == p.normalized);
    CHECK(copperArcSound() == d);
    checkReachPartition(d, "copper arc");
}
}

void ordinary_drum_local_entrypoint() {
    begin("ordinary_drum_local_entrypoint");
    const auto tile=airack::test::frozenTileArrayRackDesign();
    CHECK(validateRackDesign(tile,RackValidationMode::portableDesign));
    CHECK(tile.units.size()==1&&tile.units[0].module==NativeModule::drum);
    const auto examples=airack::test::frozenExampleRackDesigns();const auto factories=airack::test::frozenFactoryRackDesigns();
    CHECK(examples.size()==6&&factories.size()==21);
    const auto library=airack::test::frozenLibraryRackDesigns();
    CHECK(std::all_of(examples.begin(),examples.end(),[&](const auto& design){return std::find(library.begin(),library.end(),design)!=library.end();}));
    CHECK(std::all_of(factories.begin(),factories.end(),[&](const auto& design){return std::find(library.begin(),library.end(),design)!=library.end();}));
    CHECK(std::count_if(library.begin(),library.end(),[&](const auto& design){return design==tile;})==1);
}
void factory_sound_collection() {
    begin("factory_sound_collection");
    const auto sounds=factorySounds();CHECK(sounds.size()==12);
    juce::StringArray names, hashes;
    std::vector<ParameterValues> controls;
    for(std::size_t i=0;i<12;++i){
        SoundDocument sound;CHECK(makeFactorySound(i,sound));
        if(i>=sounds.size())continue;
        CHECK(juce::String(sounds[i].name).isNotEmpty()&&juce::String(sounds[i].description).isNotEmpty());
        CHECK(!names.contains(sounds[i].name));names.add(sounds[i].name);
        CHECK(sound.name==sounds[i].name&&validateSoundDocument(sound));
        CHECK(std::find(controls.begin(),controls.end(),sound.values)==controls.end());controls.push_back(sound.values);
        CHECK(sound.personalities&&sound.personalities->instances.size()==1);
        CHECK(sound.layout.synthCount==1&&sound.layout.effects==newInstanceSound().layout.effects);
        juce::MemoryBlock bytes;CHECK(encodeSoundState(sound,bytes));SoundDocument recalled;
        CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==sound);
        const auto hash=juce::SHA256(bytes).toHexString();CHECK(!hashes.contains(hash));hashes.add(hash);
        SoundDocument again;CHECK(makeFactorySound(i,again)&&again==sound);
        if(sound.personalities&&!sound.personalities->instances.empty()){const auto& rack=sound.personalities->instances.front();
            CHECK(rack.design==factoryRackDesigns()[i<6?0:1]);
        }
    }
    auto preserved=copperArcSound();const auto before=preserved;
    CHECK(!makeFactorySound(sounds.size(),preserved)&&preserved==before);
}
int main(int argc,char** argv) { if(argc==2&&std::string_view(argv[1])=="--factory-sounds"){factory_sound_collection();std::cout<<checks<<" CHECKs, "<<failures<<" failures\n";return failures?1:0;} factory_sound_collection(); if(argc==2&&std::string_view(argv[1])=="--drum-local-entrypoint"){ordinary_drum_local_entrypoint();std::cout<<checks<<" CHECKs, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;} if(catalog_denial_diagnostic_if_requested()){std::cout<<checks<<" CHECKs, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;} display_names();library_designs();oxide_return_design();small_hours_design();night_dial_design();user_reach();new_instance_sound();catalog_existing_codec_resource_calibration();catalog_all_pages_custom_identity();catalog_fresh_ownership_and_resource_fences();mid_side_split_old_1175_evidence();effect_mid_side_split_factory_availability();effect_mid_side_split_actual_placement_and_recall();frequency_split_old_1111_evidence();effect_frequency_split_factory_availability();effect_frequency_split_actual_placement_and_recall();convolution_old_1079_evidence();effect_convolution_factory_availability();effect_convolution_actual_placement_and_recall();frequency_shift_old_1055_evidence();effect_frequency_shift_factory_availability();effect_frequency_shift_actual_placement_and_recall();dimension_old_1023_evidence();effect_dimension_factory_availability();effect_dimension_actual_placement_and_recall();stereo_old_991_evidence();effect_stereo_factory_availability();effect_stereo_actual_placement_and_recall();flanger_old_951_evidence();effect_flanger_factory_availability();effect_flanger_actual_placement_and_recall();phaser_old_911_evidence();effect_phaser_factory_availability();effect_phaser_actual_placement_and_recall(); effect_chorus_actual_placement_and_recall();chorus_old_871_evidence(); effect_chorus_factory_availability(); effect_filter_actual_placement_and_recall(); filter_old_831_evidence(); effect_filter_factory_availability(); effect_eq_old_registry_and_state_evidence(); effect_eq_factory_and_control_authority(); core_fx_preserves_existing_factory_definitions(); factory_core_fx_placement_and_recall(NativeModule::compressor); factory_core_fx_placement_and_recall(NativeModule::drive); factory_core_fx_placement_and_recall(NativeModule::delay); factory_reverb_placement_and_recall(); factory_placement_and_recall(false); factory_placement_and_recall(true); superseded_factory_synth_recall_and_catalog(false); superseded_factory_synth_recall_and_catalog(true); superseded_factory_drive_recall_and_catalog(); superseded_factory_compressor_recall_and_catalog(); for(const auto& entry:supersededFactoryTaps)superseded_factory_tap_recall_and_catalog(entry); factory_synth_sustain_matches_superseded(false); factory_synth_sustain_matches_superseded(true); factory_drive_sustain_matches_superseded(); factory_compressor_sustain_matches_superseded(); for(const auto& entry:supersededFactoryTaps)factory_tap_sustain_matches_superseded(entry); algorithm_revision_transaction(); modern_reverb_portable_admission(); active_parameter_policy(); drum_module_and_compatibility(); utf8_and_recipe_bounds(); revision_decimal_roundtrip(); portable_semantics(); stable_registry(); legal_physical_endpoint_roundtrip(); std::cout << checks << " CHECKs, " << sections << " sections, " << failures << " failures\n"; return failures ? 1 : 0; }
