#include "PatternDocument.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

namespace
{
using namespace airack;
using Kind = PatternEditKind;
using Result = PatternEditResult;
int checks = 0, failures = 0, sections = 0;
void check(bool good, const char* what, int line)
{
    ++checks;
    if (!good) { ++failures; std::cout << "FAIL line " << line << ": " << what << '\n'; }
}
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
void section(const char* name) { ++sections; std::cout << "SECTION " << name << '\n'; }

constexpr PatternNote first { 2, .5, 72, 101, 3 };
constexpr PatternNote second { 0, .125, 0, 1, 1 };
constexpr PatternNote third { 1.25, 2.75, 127, 127, 16 };
PatternDocument populated()
{
    PatternDocument document;
    document.noteCount = 3;
    document.notes = { first, second, third };
    return document;
}

// Capture actual object bytes, including padding, before the call. Equality alone
// cannot prove preservation for a rejected document containing NaN.
auto bytes(const PatternDocument& document)
{
    std::array<unsigned char, sizeof(PatternDocument)> result {};
    std::memcpy(result.data(), &document, result.size());
    return result;
}
void rejected(const PatternDocument& source, PatternEdit edit, Result want = Result::invalid)
{
    auto output = populated();
    output.swing = .75;
    const auto outputBefore = bytes(output);
    const auto sourceBefore = bytes(source);
    CHECK(editPatternDocument(source, edit, output) == want);
    CHECK(bytes(output) == outputBefore);
    CHECK(bytes(source) == sourceBefore);
    auto alias = source;
    const auto aliasBefore = bytes(alias);
    CHECK(editPatternDocument(alias, edit, alias) == want);
    CHECK(bytes(alias) == aliasBefore);
}
void accepted(const PatternDocument& source, PatternEdit edit,
              const PatternDocument& expected, Result want = Result::applied)
{
    auto output = populated();
    const auto sourceBefore = bytes(source);
    CHECK(editPatternDocument(source, edit, output) == want);
    CHECK(output == expected);
    CHECK(bytes(source) == sourceBefore);
    auto alias = source;
    CHECK(editPatternDocument(alias, edit, alias) == want);
    CHECK(alias == expected);
    CHECK(validatePatternDocument(output));
}
void invalidDocument(const PatternDocument& document)
{
    CHECK(!validatePatternDocument(document));
    for (auto kind : { Kind::addNote, Kind::replaceNote, Kind::removeNote, Kind::setSettings })
        rejected(document, { kind });
}
void invalidNote(PatternNote note)
{
    PatternDocument document;
    document.noteCount = 1;
    document.notes[0] = note;
    invalidDocument(document);
    rejected(PatternDocument {}, { Kind::addNote, 0, note });
    rejected(populated(), { Kind::replaceNote, 1, note });
}

void default_empty_and_explicit_meter()
{
    section("default_empty_and_explicit_meter");
    // Catches wrong beat units/defaults, inherited old contents, or host-like meter rewriting.
    const PatternDocument defaults;
    CHECK(validatePatternDocument(defaults));
    CHECK(patternLengthBeats(defaults) == 4);
    CHECK(defaults.notes == std::array<PatternNote, 256> {});
    CHECK(defaults.noteCount == 0 && defaults.grid == PatternGrid::sixteenth && defaults.swing == 0);
    CHECK(defaults.meter == PhraseMeter { 4, 4 } && defaults.bars == 1);
    struct MeterCase { PhraseMeter meter; std::uint8_t bars; double beats; };
    for (const auto test : { MeterCase {{1, 32}, 1, .125}, {{7, 8}, 1, 3.5},
                            {{32, 1}, 8, 1024}, {{3, 2}, 2, 12}, {{5, 16}, 3, 3.75} })
    {
        auto output = populated();
        CHECK(makeEmptyPattern(test.meter, output));
        PatternDocument expected;
        expected.meter = test.meter;
        CHECK(output == expected);
        output.bars = test.bars;
        CHECK(validatePatternDocument(output));
        CHECK(patternLengthBeats(output) == test.beats);
        const auto copy = output;
        CHECK(copy == output && copy.meter == test.meter);
    }
    const std::array<double, 5> gridBeats { 1, .5, .25, .125, .0625 };
    for (std::size_t i = 0; i < gridBeats.size(); ++i)
    {
        auto document = defaults;
        document.grid = static_cast<PatternGrid>(i);
        document.swing = 1;
        CHECK(patternGridBeats(document.grid) == gridBeats[i]);
        CHECK(validatePatternDocument(document));
        CHECK(patternLengthBeats(document) == 4);
    }
}

void invalid_settings_and_all_note_fields()
{
    section("invalid_settings_and_all_note_fields");
    // Catches omitted field/enum/finite checks, inactive hidden data and repairing invalid sources.
    const auto invalidSettings = [](const PatternDocument& document) {
        invalidDocument(document);
        PatternEdit edit { Kind::setSettings };
        edit.meter = document.meter; edit.bars = document.bars;
        edit.grid = document.grid; edit.swing = document.swing;
        rejected(populated(), edit);
    };
    for (int numerator = 0; numerator <= 255; ++numerator)
    {
        const PhraseMeter meter { static_cast<std::uint8_t>(numerator), 4 };
        CHECK(validatePhraseMeter(meter) == (numerator >= 1 && numerator <= 32));
        if (numerator >= 1 && numerator <= 32) continue;
        PatternDocument document; document.meter = meter;
        CHECK(patternLengthBeats(document) == 0);
        invalidSettings(document);
        auto output = populated(); const auto before = bytes(output);
        CHECK(!makeEmptyPattern(meter, output)); CHECK(bytes(output) == before);
    }
    for (int denominator = 0; denominator <= 255; ++denominator)
    {
        const bool valid = denominator == 1 || denominator == 2 || denominator == 4
                        || denominator == 8 || denominator == 16 || denominator == 32;
        const PhraseMeter meter { 7, static_cast<std::uint8_t>(denominator) };
        CHECK(validatePhraseMeter(meter) == valid);
        if (valid) continue;
        PatternDocument document; document.meter = meter;
        CHECK(patternLengthBeats(document) == 0);
        invalidSettings(document);
        auto output = populated(); const auto before = bytes(output);
        CHECK(!makeEmptyPattern(meter, output)); CHECK(bytes(output) == before);
    }
    for (int bars = 0; bars <= 255; ++bars)
    {
        PatternDocument document; document.bars = static_cast<std::uint8_t>(bars);
        if (bars >= 1 && bars <= 8) { CHECK(validatePatternDocument(document)); continue; }
        CHECK(patternLengthBeats(document) == 0);
        invalidSettings(document);
    }
    for (int value = 5; value <= 255; ++value)
    {
        PatternDocument document; document.grid = static_cast<PatternGrid>(value);
        CHECK(patternGridBeats(document.grid) == 0);
        CHECK(patternLengthBeats(document) == 4);
        invalidSettings(document);
    }
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    for (double value : { -.1, std::nextafter(1., 2.), nan, inf, -inf })
    {
        PatternDocument document; document.swing = value; invalidSettings(document);
    }
    for (double value : { -.1, 4., std::numeric_limits<double>::max(), nan, inf, -inf })
    { auto note = first; note.startBeats = value; invalidNote(note); }
    for (double value : { 0., -.1, std::nextafter(2., 3.), std::numeric_limits<double>::max(), nan, inf, -inf })
    { auto note = first; note.durationBeats = value; invalidNote(note); }
    for (int value = 128; value <= 255; ++value)
    { auto note = first; note.pitch = static_cast<std::uint8_t>(value); invalidNote(note); }
    for (int value = 0; value <= 255; ++value)
    {
        if (value < 1 || value > 127)
        { auto note = first; note.velocity = static_cast<std::uint8_t>(value); invalidNote(note); }
        if (value < 1 || value > 16)
        { auto note = first; note.channel = static_cast<std::uint8_t>(value); invalidNote(note); }
    }
    for (std::size_t index : { std::size_t {0}, std::size_t {3}, std::size_t {255} })
    {
        PatternDocument document; document.notes[index].pitch = 61; invalidDocument(document);
        document = {}; document.notes[index].startBeats = nan; invalidDocument(document);
        document = {}; document.notes[index].durationBeats = inf; invalidDocument(document);
        document = {}; document.notes[index].velocity = 99; invalidDocument(document);
        document = {}; document.notes[index].channel = 2; invalidDocument(document);
    }
    auto document = populated(); document.notes[3] = first; invalidDocument(document);
}

void capacity_256_and_overflow()
{
    section("capacity_256_and_overflow");
    // Catches off-by-one capacity, count indexing before validation and wrong error precedence.
    PatternDocument full; full.noteCount = 256; full.notes.fill(first);
    CHECK(validatePatternDocument(full));
    auto almost = full; almost.noteCount = 255; almost.notes[255] = {};
    accepted(almost, { Kind::addNote, 65535, first }, full);
    rejected(full, { Kind::addNote, 0, first }, Result::capacity);
    auto bad = first; bad.velocity = 0;
    rejected(full, { Kind::addNote, 0, bad });
    for (auto count : { 257, 65535 })
    { auto corrupt = full; corrupt.noteCount = static_cast<std::uint16_t>(count); invalidDocument(corrupt); }
    for (std::size_t index = 0; index < 256; ++index)
    { auto corrupt = full; corrupt.notes[index].channel = 0; invalidDocument(corrupt); }
    auto expected = full; expected.notes[255] = second;
    accepted(full, { Kind::replaceNote, 255, second }, expected);
    expected = full; expected.noteCount = 255; expected.notes[255] = {};
    accepted(full, { Kind::removeNote, 255 }, expected);
}

void transactional_alias_and_failure_preservation()
{
    section("transactional_alias_and_failure_preservation");
    // Catches early output assignment, alias corruption and not publishing valid no-ops.
    const auto source = populated();
    accepted(source, { Kind::replaceNote, 1, second }, source, Result::unchanged);
    accepted(source, { Kind::setSettings }, source, Result::unchanged);
    for (auto index : { 3, 256, 65535 })
    {
        rejected(source, { Kind::replaceNote, static_cast<std::uint16_t>(index), first });
        rejected(source, { Kind::removeNote, static_cast<std::uint16_t>(index) });
    }
    rejected(PatternDocument {}, { Kind::removeNote, 0 });
    rejected(PatternDocument {}, { Kind::replaceNote, 0, first });
    for (int kind = 4; kind <= 255; ++kind)
        rejected(source, { static_cast<Kind>(kind) });
    // Fields irrelevant to the selected operation do not cause rejection.
    PatternEdit append { Kind::addNote, 65535, second, {0, 0}, 0, static_cast<PatternGrid>(255), -1 };
    auto expected = source; expected.noteCount = 4; expected.notes[3] = second;
    accepted(source, append, expected);
    PatternEdit remove { Kind::removeNote, 1 }; remove.note.durationBeats = -1;
    expected = source; expected.noteCount = 2; expected.notes = { first, third };
    accepted(source, remove, expected);
}

void append_replace_remove_preserves_note_order()
{
    section("append_replace_remove_preserves_note_order");
    // Catches sorting/quantizing notes, incomplete replacement and failure to compact/clear suffix.
    PatternDocument empty, one, two;
    one.noteCount = 1; one.notes = { first };
    two.noteCount = 2; two.notes = { first, second };
    accepted(empty, { Kind::addNote, 0, first }, one);
    accepted(one, { Kind::addNote, 0, second }, two);
    accepted(two, { Kind::addNote, 0, third }, populated());
    auto expected = populated(); expected.notes = { first, PatternNote { .375, .0625, 55, 42, 9 }, third };
    accepted(populated(), { Kind::replaceNote, 1, { .375, .0625, 55, 42, 9 } }, expected);
    expected = {}; expected.noteCount = 2; expected.notes = { second, third };
    accepted(populated(), { Kind::removeNote, 0 }, expected);
    expected.notes = { first, third };
    accepted(populated(), { Kind::removeNote, 1 }, expected);
    expected.notes = { first, second };
    accepted(populated(), { Kind::removeNote, 2 }, expected);
    accepted(one, { Kind::removeNote, 0 }, empty);
}

void settings_preserve_notes_or_reject_shrink()
{
    section("settings_preserve_notes_or_reject_shrink");
    // Catches destructive shortening, rescaling/quantizing on meter/grid/swing edits, and lost meter.
    const auto source = populated();
    PatternEdit settings { Kind::setSettings, 65535, {}, {7, 8}, 2, PatternGrid::sixtyFourth, .875 };
    auto expected = source; expected.meter = {7, 8}; expected.bars = 2;
    expected.grid = PatternGrid::sixtyFourth; expected.swing = .875;
    accepted(source, settings, expected);
    CHECK(patternLengthBeats(expected) == 7);
    auto copy = expected;
    PatternEdit append { Kind::addNote, 0, { 6.5, .5, 45, 64, 5 } };
    auto appended = expected; appended.noteCount = 4; appended.notes[3] = append.note;
    accepted(copy, append, appended);
    settings.bars = 1; rejected(source, settings);
    settings.meter = {1, 32}; rejected(source, settings);
    settings.meter = {4, 4}; settings.grid = PatternGrid::quarter; settings.swing = 1;
    expected = source; expected.grid = PatternGrid::quarter; expected.swing = 1;
    accepted(source, settings, expected);
    auto longSource = source; longSource.bars = 8;
    expected = source;
    accepted(longSource, { Kind::setSettings }, expected);
    longSource.notes[0] = {31, 1, 70, 80, 2};
    rejected(longSource, { Kind::setSettings });
    PatternDocument tiny; tiny.meter = {1, 32};
    accepted(PatternDocument {}, { Kind::setSettings, 0, {}, {1, 32} }, tiny);
}

void overlap_and_exact_phrase_end()
{
    section("overlap_and_exact_phrase_end");
    // Catches end >= rejection, rounded-addition overrun acceptance, and overlap/duration policies.
    PatternDocument document; document.meter = {7, 8}; document.noteCount = 4;
    document.notes = { PatternNote {0, 3.5, 60, 100, 1}, PatternNote {0, 3.5, 60, 100, 1},
                       PatternNote {0, 3.5, 64, 80, 1}, PatternNote {3.25, .25, 60, 127, 1} };
    CHECK(validatePatternDocument(document));
    auto expected = document; expected.noteCount = 5;
    expected.notes[4] = {3.25, .25, 60, 127, 1};
    accepted(document, {Kind::addNote, 0, expected.notes[4]}, expected);
    auto overrun = document; overrun.notes[3].durationBeats = std::nextafter(.25, 1.);
    invalidDocument(overrun);
    overrun = document; overrun.notes[3].startBeats = std::nextafter(3.25, 4.);
    invalidDocument(overrun);
    overrun = document; overrun.notes[3].startBeats = 3.5;
    overrun.notes[3].durationBeats = std::numeric_limits<double>::denorm_min();
    invalidDocument(overrun);
    PatternDocument tiny; tiny.meter = {1, 32}; tiny.noteCount = 1;
    tiny.notes[0] = {0, .125, 0, 1, 16}; CHECK(validatePatternDocument(tiny));
    tiny.notes[0].durationBeats = std::nextafter(.125, 1.); invalidDocument(tiny);
    tiny.notes[0] = {std::nextafter(.125, 0.), std::numeric_limits<double>::denorm_min(), 127, 127, 1};
    CHECK(validatePatternDocument(tiny));
    PatternDocument largest; largest.meter = {32, 1}; largest.bars = 8; largest.noteCount = 1;
    largest.notes[0] = {1023, 1, 60, 100, 1}; CHECK(validatePatternDocument(largest));
    largest.notes[0].durationBeats = std::nextafter(1., 2.); invalidDocument(largest);
}

void fixed_storage_and_noexcept()
{
    section("fixed_storage_and_noexcept");
    // Catches introducing dynamic storage or throwing edit/validation contracts.
    static_assert(maximumPatternNotes == 256);
    static_assert(std::is_same_v<decltype(PatternDocument::notes), std::array<PatternNote, 256>>);
    static_assert(std::is_trivially_copyable_v<PatternDocument>);
    static_assert(std::is_nothrow_copy_constructible_v<PatternDocument>);
    static_assert(std::is_nothrow_copy_assignable_v<PatternDocument>);
    static_assert(std::is_trivially_copyable_v<PatternEdit>);
    static_assert(noexcept(patternGridBeats(PatternGrid {})));
    static_assert(noexcept(validatePhraseMeter(PhraseMeter {})));
    static_assert(noexcept(patternLengthBeats(std::declval<const PatternDocument&>())));
    static_assert(noexcept(validatePatternDocument(std::declval<const PatternDocument&>())));
    static_assert(noexcept(makeEmptyPattern(PhraseMeter {}, std::declval<PatternDocument&>())));
    static_assert(noexcept(editPatternDocument(std::declval<const PatternDocument&>(),
                                              PatternEdit {}, std::declval<PatternDocument&>())));
    PatternDocument source; source.noteCount = 256; source.notes.fill(first);
    auto expected = source; expected.notes[0] = third;
    accepted(source, {Kind::replaceNote, 0, third}, expected);
}
}

int main()
{
    default_empty_and_explicit_meter();
    invalid_settings_and_all_note_fields();
    capacity_256_and_overflow();
    transactional_alias_and_failure_preservation();
    append_replace_remove_preserves_note_order();
    settings_preserve_notes_or_reject_shrink();
    overlap_and_exact_phrase_end();
    fixed_storage_and_noexcept();
    std::cout << checks << " checks in " << sections << " sections, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
