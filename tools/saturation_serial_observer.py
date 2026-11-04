"""Removable normal-Candidate serial observer. Never generates expected audio."""
import argparse
import ast
from collections import Counter
from itertools import groupby
import hashlib
import json
import math
from pathlib import Path
import re
import struct

import saturation_fixture_revision as revision

ROOT = Path(__file__).resolve().parents[1]
BEGIN = b'\n/* AIRACK_SERIAL_OBSERVER_BEGIN */\n'
END = b'\n/* AIRACK_SERIAL_OBSERVER_END */\n'
PARTITIONS = (1, 17, 64, 127, 512)
TRACE_LIMIT = 1024*1024*1024
SAVED = {
    'Release': ('attempt-7f7a90894e654e2a95d2b079279e857e', '5ac225a2dcf34ebfd71800dff88d018e68267c8d6d373565fa8d6223bee2aac6'),
    'Debug': ('attempt-8f5acb1e210b48598f4e01ec7e927bca', 'b25b0d1f24ce101cb355a43c091c8a3a619e80cb6daf7f7d4402218f8bd0d0b4'),
}
INT_FIELDS = ('partition', 'frame', 'slot', 'stage', 'order', 'channel', 'lane',
              'subsample', 'call', 'kind', 'selector', 'object', 'flags', 'ramp')
KEY_FIELDS = INT_FIELDS[:9] + ('object',)
WIDTHS = {0: 0, 1: 0, 2: 6, 3: 18, 4: 8, 5: 5, 6: 3, 7: 1, 8: 2, 9: 2,
          10: 4, 11: 4, 12: 8, 13: 8, 14: 4, 15: 0, 16: 7, 20: 8, 21: 3,
          30: 10, 31: 4, 32: 2, 33: 5, 34: 4, 35: 7, 40: 3, 41: 2, 42: 3,
          50: 2, 51: 2}
# Current protected design/JUCE layout; changed design dependencies require review.
RESOURCE_EXTENTS = (5,4,5,4,3,3,3,3,2,2,2,2,257,129,65,33,17,17,4096,768)


def require(ok, message):
    if not ok:
        raise ValueError(message)


def recover(generated: bytes) -> bytes:
    require(generated.count(BEGIN) == generated.count(END), 'unbalanced observer markers')
    result = re.sub(re.escape(BEGIN) + b'(?:(?!' + re.escape(BEGIN) + b').)*?' + re.escape(END),
                    b'', generated, flags=re.DOTALL)
    require(BEGIN not in result and END not in result, 'malformed observer markers')
    return result


def insertions(original, edits):
    require(BEGIN not in original and END not in original, 'source already instrumented')
    generated = original
    for at, code in sorted(edits, key=lambda e: e[0], reverse=True):
        generated = generated[:at] + BEGIN + code.encode() + END + generated[at:]
    require(recover(generated) == original, 'byte-exact insertion recovery required')
    return generated


def generate_candidate(original: bytes) -> bytes:
    newline = '\r\n' if b'\r\n' in original else '\n'
    edits = []

    def add(anchor, code, count=1, section=None, before=False):
        needle = anchor.replace('\n', newline).encode()
        start, end = (0, len(original)) if section is None else section
        require(original[start:end].count(needle) == count, 'unique observer anchor: '+anchor[:80])
        at = start
        for _ in range(count):
            at = original.index(needle, at)
            edits.append((at if before else at+len(needle), code))
            at += len(needle)

    def section(start, end, bounds=None):
        lo, hi = (0,len(original)) if bounds is None else bounds
        require(original[lo:hi].count(start.encode()) == 1 and original[lo:hi].count(end.encode()) == 1,
                'unique observer section')
        return original.index(start.encode(),lo,hi), original.index(end.encode(),lo,hi)

    def wrap(anchor, expression, prefix):
        needle = anchor.encode()
        require(original.count(needle) == 1 and needle.count(expression.encode()) == 1,
                'unique actual return expression')
        at = original.index(needle) + needle.index(expression.encode())
        edits.extend(((at,prefix),(at+len(expression.encode()),')')))

    add('#include <vector>', '#include "SaturationSerialObserver.h"\n#include "SaturationSerialSource.h"\n'
        '#if defined(AIRACK_SATURATION_NUMERICAL_TEST) || defined(AIRACK_SATURATION_TRANSITIONS) || defined(AIRACK_SATURATION_REFERENCE_TRACE)\n'
        '#error Serial observer requires the normal untransitioned Candidate\n#endif')
    wrap('double gain(double db) noexcept { return gain_math::analyticGain(db); }',
         'gain_math::analyticGain(db)', 'serial_observer::gainResult(50, 0, db, ')
    wrap('double gainForFactor(double db) const noexcept { return factor == 4 ? coefficients->gainCurve.gain(db) : gain(db); }',
         'factor == 4 ? coefficients->gainCurve.gain(db) : gain(db)', 'serial_observer::gainResult(51, factor, db, ')
    add('double cachedShape(SaturationCharacter c, double x) noexcept {', 'serial_observer::shapeInput(x);')
    add('double exactQuotient(SaturationCharacter c, double x, double p, double bias, double normalizer) noexcept {',
        'serial_observer::exactInput(x, p, bias, normalizer);')
    normal = section('Candidate::Candidate() : state(std::make_unique<State>()) {}',
                     'std::span<const SaturationNumericalRecord> saturationNumericalRecords()')
    add('bool Candidate::prepare(double rate, unsigned factor) noexcept {',
        'serial_observer::candidate(this); serial_observer::compiledSource(RACKSYNTH_SERIAL_SOURCE_SHA256);', section=normal)
    add('bool Candidate::setSettings(const SaturationSettings& next) noexcept {',
        'serial_observer::candidate(this);', section=normal)
    settings = section('bool Candidate::setSettings(const SaturationSettings& next) noexcept {',
                       'Candidate::Frame Candidate::process(Frame input) noexcept {', normal)
    add('    return true;', 'serial_observer::state(*state);', section=settings, before=True)
    add('state.swap(replacement);',
        'serial_observer::ready(*state, preparedConstants.logTwo, preparedConstants.sqrtTwo, preparedConstants.glowBias, R, pi, gain_math::GainCurve::decibelsToNaturalLog);', section=normal)
    add('Candidate::Frame Candidate::process(Frame input) noexcept {',
        'serial_observer::candidate(this); serial_observer::frameValue(8, input);', section=normal)
    add('void Candidate::reset() noexcept {', 'serial_observer::candidate(this);', section=normal)
    add('coefficientSettings = current; coefficientCorners = corners; coefficientsValid = true;',
        'serial_observer::state(*this);')
    add('        std::array<Frame, Capacity> wet, dry, output;',
        'serial_observer::bind(s); serial_observer::state(s);', before=True)
    add('        if constexpr (Capacity == 4) {\n            s.wetUp.up4',
        'serial_observer::pre(input, wetInput);', before=True)
    add('        std::array<std::array<wide_network::Quad, 3>, Capacity == 4 ? 4 : 0> wideBands;',
        'if constexpr (Capacity == 4) serial_observer::up(wet, dry, s);')
    add('        std::array<std::array<Frame, 4>, Capacity == 4 ? 3 : 0> batchWet;',
        'if constexpr (Capacity == 4) serial_observer::wideAfter(wideBands, s);')
    add('            Frame w {}, d {};', 'serial_observer::point(int(n));')
    add('            valid = valid && finite(w) && finite(d) && finite(output[n]);',
        'serial_observer::mix(w, d, output[n]);')
    add('        if constexpr (Capacity == 4) returnGain(result, s.outputGain);',
        'serial_observer::downInput(result);', before=True)
    add('        else { result[0] *= s.outputGain; result[1] *= s.outputGain; }',
        'serial_observer::downOutput(result, s.outputGain); serial_observer::frameValue(9, result);')

    biquad = section('struct Biquad {', 'struct Split {')
    add('Candidate::Frame process(Candidate::Frame x, const Coefficients& c) noexcept {',
        'serial_observer::biquadIn(this, x, c, z1, z2);', section=biquad)
    add('y.copyToRawArray(samples.data());',
        'serial_observer::biquadOut(this, samples, z1, z2);', section=biquad)
    add('        return x;', 'serial_observer::biquadOut(this, x, z1, z2);', count=2,
        section=biquad, before=True)

    lane = section('    void process4Character(', '    void process4(')
    add('            auto& frame = frames[n];',
        'serial_observer::lanePoint(this, int(&frame - frames.data()));', section=lane)
    packed = section('            if (pairedCurve) {', '                continue;', lane)
    add('                _mm_storeu_pd(dc.data(), nextDc); _mm_storeu_pd(previousDifference.data(), difference);',
        '''Candidate::Frame serialD, serialT, serialDelta, serialShaped, serialBlendDifference, serialResult;
_mm_storeu_pd(serialD.data(), d); _mm_storeu_pd(serialT.data(), t);
_mm_storeu_pd(serialDelta.data(), delta); _mm_storeu_pd(serialShaped.data(), shaped);
_mm_storeu_pd(serialBlendDifference.data(), blendDifference); _mm_storeu_pd(serialResult.data(), result);
// Both channels observe one packed instruction, in logical channel order.
for (std::size_t ch = 0; ch < 2; ++ch) {
    serial_observer::laneInput(this, int(ch), int(Character), x[ch], h[ch], u[ch], previousX[ch], previousH[ch], previousU[ch], previousPrimitive[ch], previousDifference[ch], dc[ch], envelope, int(primitiveKind[ch]), s.bypassed, tilted);
    serial_observer::primitive(int(ch), int(Character), int(PrimitiveKind::curve), 0, 0, u[ch], currentPrimitive[ch]);
    serial_observer::primitive(int(ch), int(Character), int(PrimitiveKind::curve), 1, primitiveKind[ch] == PrimitiveKind::curve ? 2 : 1, previousU[ch], priorPrimitive[ch]);
    serial_observer::laneQuotient(int(ch), int(Character), int(PrimitiveKind::curve), int(primitiveKind[ch]), u[ch], previousU[ch], currentPrimitive[ch], priorPrimitive[ch], serialDelta[ch], serialShaped[ch]);
    serial_observer::laneMix(int(ch), int(Character), serialD[ch], serialT[ch], amount, serialBlendDifference[ch], wetGain, mix, serialResult[ch]);
}''', section=packed, before=True)
    add('                double shaped;',
        'serial_observer::laneInput(this, int(ch), int(Character), x[ch], h[ch], u[ch], previousX[ch], previousH[ch], previousU[ch], previousPrimitive[ch], previousDifference[ch], dc[ch], envelope, int(primitiveKind[ch]), s.bypassed, tilted);', section=lane)
    add('shaped = cachedShape(Character, .5 * (u[ch] + previousU[ch])); primitiveKind[ch] = PrimitiveKind::none;',
        'serial_observer::midpoint(int(ch), int(Character), u[ch], previousU[ch], shaped);', section=lane)
    add('const double priorPrimitive = needsPrimitive',
        'serial_observer::primitive(int(ch), int(Character), int(kind), 0, 0, u[ch], currentPrimitive);', section=lane, before=True)
    add('const double priorPrimitive = needsPrimitive\n                        ? (primitiveKind[ch] == kind ? previousPrimitive[ch] : evaluate(previousU[ch])) : 0.;',
        'serial_observer::primitive(int(ch), int(Character), int(kind), 1, primitiveKind[ch] == kind ? 2 : 1, previousU[ch], priorPrimitive);', section=lane)
    add('                        : (currentPrimitive - priorPrimitive) / delta;',
        'serial_observer::laneQuotient(int(ch), int(Character), int(kind), int(primitiveKind[ch]), u[ch], previousU[ch], currentPrimitive, priorPrimitive, delta, shaped);', section=lane)
    add('result[ch] = d + mix * ((t + amount * difference) * wetGain - d);',
        'serial_observer::laneMix(int(ch), int(Character), d, t, amount, difference, wetGain, mix, result[ch]);', section=lane)
    return insertions(original, edits)


DRIVER_MAIN = r'''
#undef main
int main(int argc, char** argv) {
    try {
        require(argc == 3 && std::string_view(argv[1]) == "--output", "usage: --output FRESH_ABSOLUTE_DIRECTORY");
        const fs::path output = argv[2];
        serial_observer::fresh(output);
        const auto descriptor = readFile(RACKSYNTH_SERIAL_SOURCE_DESCRIPTOR, 1048576);
        require(digest(descriptor) == RACKSYNTH_SERIAL_SOURCE_SHA256, "compiled source descriptor changed");
        serial_observer::expectedSource = RACKSYNTH_SERIAL_SOURCE_SHA256;
        const auto c = load(RACKSYNTH_SATURATION_FIXTURE_DIR);
        require(fs::create_directories(output), "output: cannot create directory");
        for (const auto partition : partitions) {
            serial_observer::start(int(partition));
            const auto audio = runSerial(c, partition);
            std::string bytes;
            for (const auto& frame : audio) for (const auto sample : frame) {
                const auto bits = std::bit_cast<std::uint64_t>(sample);
                for (unsigned n = 0; n < 8; ++n) bytes.push_back(char((bits >> (8*n)) & 255));
            }
            writeFile(output / ("serial-characterization.partition-" + std::to_string(partition) + ".actual.f64"), bytes);
        }
        serial_observer::write(output / "trace.jsonl");
        require(readFile(RACKSYNTH_SERIAL_SOURCE_DESCRIPTOR, 1048576) == descriptor, "source descriptor changed");
        std::ostringstream receipt;
        receipt << "{\"schema\":1,\"profile\":\""
#ifdef _DEBUG
            << "Debug"
#else
            << "Release"
#endif
            << "\",\"source_descriptor_sha256\":\"" << digest(descriptor) << "\",\"files\":{";
        bool first = true;
        for (const auto& entry : fs::directory_iterator(output)) {
            if (!first) receipt << ','; first = false;
            const auto raw = readFile(entry.path(), 1073741824);
            receipt << '"' << entry.path().filename().string() << "\":{\"bytes\":" << raw.size()
                << ",\"sha256\":\"" << digest(raw) << "\"}";
        }
        receipt << "}}\n";
        writeFile(output / "capture.json", receipt.str());
        std::cout << "SERIAL_OBSERVER complete_partitions=5 prefix=0..510 CPU=DEFERRED adoptable=false production=FORCED_BYPASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SERIAL_OBSERVER_FAILURE " << error.what() << '\n'; return 1;
    }
}
'''


def generate_driver(original: bytes) -> bytes:
    newline = b'\r\n' if b'\r\n' in original else b'\n'
    edits = [(0, '#include "SaturationSerialObserver.h"\n#include "SaturationSerialSource.h"\n#define main saturation_original_fixture_main'),
             (len(original), DRIVER_MAIN)]

    def add(anchor, code):
        needle = anchor.encode().replace(b'\n', newline)
        require(original.count(needle) == 1, 'unique driver anchor: '+anchor[:80])
        edits.append((original.index(needle)+len(needle), code))

    # The process, loop limits, settings and route branches remain original source bytes.
    add('std::array<Candidate, 8> bank;', 'serial_observer::bank(bank);')
    anchor = '        for (auto n = begin; n < std::min(begin + partition, frameCount); ++n) {\n            while (next < c.routes.size() && c.routes[next].frame == n) {'
    needle = anchor.encode().replace(b'\n', newline)
    require(original.count(needle) == 1, 'unique original serial frame loop')
    at = original.index(needle) + needle.index(b'{') + 1
    edits.append((at, 'serial_observer::frame(int(n));'))
    add('const auto& e = c.routes[next++];', 'serial_observer::event(int(e.slot), e.operation == "settings" ? 1 : e.operation == "reset" ? 2 : 0, e.bypassed);')
    return insertions(original, edits)


def qualify_audio(actual: bytes, saved: bytes) -> dict:
    revision.audio(actual); revision.audio(saved)
    require(actual == saved, 'observer full audio is not byte identical to saved profile')
    return dict(byte_identical=True, frames=2048, **revision.identity(actual))


def qualify_partitions(actual, saved):
    require(set(actual) == set(saved) == set(PARTITIONS) and
            all(type(p) is int for p in (*actual, *saved)), 'complete five partitions required')
    return {p: qualify_audio(actual[p], saved[p]) for p in PARTITIONS}


def source_inventory():
    revision.load_revision(revision.REVISION_DIR)
    paths = set((ROOT/'probes/saturation').glob('*'))
    paths.update((ROOT/'probes/saturation/contracts').glob('*.h'))
    paths.add(ROOT/'probes/saturation/contracts/SaturationFixtureTests.cpp')
    paths.add(ROOT/'CMakeLists.txt')
    for directory, pin in ((revision.LEGACY_DIR, revision.LEGACY_MANIFEST_PIN),
                           (revision.REVISION_DIR, revision.MANIFEST_PIN)):
        manifest = revision.read_regular(directory/'manifest.json', 65536)
        require(revision.identity(manifest)['sha256'] == pin, 'external corpus pin mismatch')
        data = revision.json_bytes(manifest)
        revision.members(directory, {*data['files'], 'manifest.json'})
        for name, bound in data['files'].items():
            require(revision.identity(revision.read_regular(directory/name, 1048576)) == bound,
                    'frozen corpus payload mismatch: '+name)
        paths.update(directory.iterdir())
    paths.update((Path(__file__).resolve(), ROOT/'tools/saturation_fixture_revision.py'))
    return {p.relative_to(ROOT).as_posix(): revision.identity(revision.read_regular(p, 1048576))
            for p in sorted(paths) if p.is_file()}


def fresh(path):
    path = Path(path)
    revision.plain_path(path)
    require(not path.exists() and not path.is_symlink(), 'fresh output required')
    return path


def generate_directory(directory):
    directory = fresh(directory)
    before = source_inventory()
    candidate = (ROOT/'probes/saturation/SaturationCandidate.cpp').read_bytes()
    fixture = (ROOT/'probes/saturation/contracts/SaturationFixtureTests.cpp').read_bytes()
    generated, driver = generate_candidate(candidate), generate_driver(fixture)
    payloads = {'SaturationCandidateObserved.cpp': generated, 'SaturationFixtureObserved.cpp': driver,
                'Candidate.original.cpp': candidate, 'Fixture.original.cpp': fixture,
                'Candidate.recovered.cpp': recover(generated), 'Fixture.recovered.cpp': recover(driver)}
    descriptor = dict(schema=1, scope='normal_untransitioned_4x_serial_observer',
                      candidate_recovered_exact=recover(generated) == candidate,
                      fixture_recovered_exact=recover(driver) == fixture,
                      sources=before, generated={n: revision.identity(v) for n,v in payloads.items()})
    require(source_inventory() == before, 'protected inputs changed during generation')
    directory.mkdir(parents=True)
    for name, data in payloads.items():
        (directory/name).write_bytes(data)
    raw = (json.dumps(descriptor, indent=2)+'\n').encode()
    (directory/'source.json').write_bytes(raw)
    (directory/'SaturationSerialSource.h').write_text(
        '#define RACKSYNTH_SERIAL_SOURCE_SHA256 "'+revision.identity(raw)['sha256']+'"\n', encoding='utf-8', newline='\n')
    return descriptor


def validate_records(records):
    require(type(records) is list and 0 < len(records) <= 2000000, 'bounded nonempty trace records required')
    seen = set()
    calls = Counter()
    for index, record in enumerate(records):
        require(type(record) is dict and set(record) == {*INT_FIELDS, 'words'}, 'exact trace schema required')
        require(all(type(record[k]) is int for k in INT_FIELDS), 'integer trace identities required')
        require(record['order'] == index and record['partition'] == 1 and -1 <= record['frame'] <= 510
                and -1 <= record['slot'] <= 8 and -1 <= record['channel'] <= 1
                and -1 <= record['lane'] <= 3 and -1 <= record['subsample'] <= 4096
                and record['stage'] in WIDTHS and record['call'] >= 0
                and -1 <= record['object'] <= 27
                and 0 <= record['kind'] <= 3 and 0 <= record['selector'] <= 15
                and 0 <= record['flags'] <= 255 and 0 <= record['ramp'] <= 960,
                'trace identity range/order required')
        stage = record['stage']
        if stage in (31,32,33,34):
            require(record['channel'] in (0,1) and record['lane'] in range(4) and
                    record['subsample'] in range(4) and record['selector'] in range(3) and
                    record['kind'] in ((1,2) if stage == 32 else ({31:0,33:1,34:2}[stage],)) and
                    record['object'] in ((0,1) if stage == 32 else (-1,)),
                    'actual lane operation identities required')
        if stage in (0,2,3,7,8,9,10,11,12,13,14,15,16,41,50,51):
            require(record['channel'] == -1 and record['lane'] == -1, 'scalar/stereo boundary identity required')
        if stage in (50,51):
            require(record['object'] == record['subsample'] == -1 and
                    all(record[k] == 0 for k in ('kind','selector','ramp')) and
                    record['flags'] == (4 if stage == 51 else 0), 'gain call actual factor identity required')
        words = record['words']
        require(type(words) is list and len(words) == WIDTHS[record['stage']], 'exact stage field count required')
        require(all(type(w) is str and re.fullmatch('[0-9a-f]{16}', w) and
                    math.isfinite(struct.unpack('>d', bytes.fromhex(w))[0]) for w in words),
                'finite binary64 hexadecimal words required')
        key = tuple(record[k] for k in KEY_FIELDS)
        require(key not in seen, 'duplicate trace record')
        seen.add(key)
        call_key = tuple(record[k] for k in ('frame', 'slot', 'stage', 'channel', 'lane', 'subsample', 'object'))
        require(record['call'] == calls[call_key], 'trace call ordering required')
        calls[call_key] += 1
    return records


def read_trace(path):
    raw = revision.read_regular(Path(path), TRACE_LIMIT)
    require(raw.endswith(b'\n'), 'trace final newline required')
    records = validate_records([revision.json_bytes(line) for line in raw.splitlines()])
    validate_coverage(records)
    return records


def validate_coverage(records):
    """Consume the source's whole group grammar, including every cross-stage edge."""
    groups = [(key,list(rows)) for key,rows in groupby(records,lambda r:(r['frame'],r['slot']))]
    expected = [(-1,s) for s in range(8)]
    for f in range(511):
        expected.append((f,-1))
        if f in (63,64,65,127):
            expected.append((f,2))
        expected.extend((f,s) for s in range(8) if not (s == 2 and 63 <= f < 127))
    require([key for key,_ in groups] == expected, 'ordered contiguous preparation/event/audio groups required')
    for (frame,slot),rows in groups:
        pos = 0

        def take(stage, lane=-1, n=-1, ch=-1, obj=-1):
            nonlocal pos
            require(pos < len(rows), 'complete execution group required')
            row = rows[pos]; pos += 1
            require(tuple(row[k] for k in ('stage','lane','subsample','channel','object')) ==
                    (stage,lane,n,ch,obj), 'complete cross-stage execution order required')
            return row

        def state():
            current = take(3); take(16)
            for obj in range(7): take(5,obj=obj)
            controls = []
            for lane in range(4):
                controls.append(take(4,lane)); take(6,lane)
                take(5,lane,obj=7+2*lane); take(5,lane,obj=8+2*lane)
            return current,controls

        def biquad(obj, lane=-1, n=-1):
            for stage in (20,21):
                for ch in range(2): take(stage,lane,n,ch,obj)

        if frame == -1:
            for stage in (51,51,50,51,51,51,51): take(stage)
            state(); take(2); state()
            if slot == 0:
                for obj,extent in enumerate(RESOURCE_EXTENTS):
                    take(15,n=extent,obj=obj)
                    for n in range(extent): take(7,n=n,obj=obj)
            for _ in range(4): take(50); take(51)
            state(); state()
        elif slot == -1:
            take(0)
        elif frame in (63,64,65,127) and slot == 2 and rows[0]['stage'] == 1:
            event = take(1)
            require((event['selector'],event['kind']) ==
                    ({63:(0,1),64:(1,0),65:(1,0),127:(0,0)}[frame]),
                    'exact prefix settings/routes required')
            if frame in (64,65): state()
        else:
            take(8)
            mode = int(slot % 2 == 1 or slot == 2 and frame >= 127)
            if slot == 2 and frame >= 127:
                for _ in range(4): take(50); take(51)
                ramp_current,_ = state()
                require(ramp_current['kind'] == mode and ramp_current['flags'] in (0,1), 'original ramp mode/filter flags required')
            current,controls = state()
            require(current['kind'] == mode and current['flags'] in (0,1), 'original serial mode/filter flags required')
            take(10)
            for n in range(4): take(11,n=n)
            wide = mode and bool(current['flags'] & 1)
            if wide:
                for stage in (12,13):
                    for obj in (0,2,4,6,1,3,5,7,8): take(stage,obj=obj)
                for n in range(4):
                    for band in range(3): take(14,n=n,obj=band)
            elif mode:
                for n in range(4):
                    for obj in (10,12,11,13,14,16,15,17,18,19,21,20,22,23,25,24,26,27): biquad(obj)
            for lane in (range(1,4) if mode else (0,)):
                tilted = struct.unpack('>d',bytes.fromhex(controls[lane]['words'][1]))[0] != 0
                for n in range(4):
                    if tilted:
                        biquad(2+2*lane,lane,n); biquad(3+2*lane,lane,n)
                    for ch in range(2):
                        begin = pos
                        lane_input = take(30,lane,n,ch)
                        require(pos < len(rows), 'actual lane branch required')
                        if rows[pos]['stage'] == 31:
                            take(31,lane,n,ch)
                        else:
                            first = take(32,lane,n,ch,0); prior = take(32,lane,n,ch,1)
                            require(pos < len(rows) and rows[pos]['stage'] in (33,34), 'actual quotient branch required')
                            quotient = take(rows[pos]['stage'],lane,n,ch)
                            require(first['kind'] == prior['kind'] == quotient['kind'] and first['flags'] == 0
                                    and lane_input['kind'] in (0,1,2) and quotient['flags'] == lane_input['kind']
                                    and prior['flags'] == (2 if lane_input['kind'] == first['kind'] else 1),
                                    'current/prior/cache identities required')
                        take(35,lane,n,ch)
                        require(all(r['selector'] == controls[lane]['kind'] for r in rows[begin:pos]),
                                'lane character/branch identity required')
            for n in range(4):
                for ch in range(2): take(40,n=n,ch=ch)
            take(41)
            for ch in range(2): take(42,ch=ch)
            take(9)
        require(pos == len(rows), 'unexpected trailing execution-group records')


def compare_traces(left, right):
    validate_records(left); validate_records(right)
    require(len(left) == len(right) and
            [tuple(r[k] for k in KEY_FIELDS) for r in left] ==
            [tuple(r[k] for k in KEY_FIELDS) for r in right], 'paired trace identity/order/coverage required')
    # Immutable resources are recorded after their first preparation use. Require
    # independently complete paired context, never just matching scalar arguments.
    equal_constants = set()
    for slot in range(8):
        a = [r for r in left if r['stage'] == 2 and r['frame'] == -1 and r['slot'] == slot]
        b = [r for r in right if r['stage'] == 2 and r['frame'] == -1 and r['slot'] == slot]
        if len(a) == 1 and a == b: equal_constants.add(slot)
    equal_resources = set()
    for obj in (18,19):
        a = [r for r in left if r['stage'] in (7,15) and r['object'] == obj]
        b = [r for r in right if r['stage'] in (7,15) and r['object'] == obj]
        identities = [(r['frame'],r['slot'],r['stage'],r['subsample']) for r in a]
        extent = RESOURCE_EXTENTS[obj]
        if identities == [(-1,0,15,extent)]+[(-1,0,7,n) for n in range(extent)] and a == b:
            equal_resources.add(obj)
    earliest = equal_operation = scalar_operation = None
    for index, (a,b) in enumerate(zip(left,right)):
        if a != b and earliest is None:
            earliest = dict(index=index, left=a, right=b)
        # Only recorded actual call/quotient operands, never shadow reevaluation.
        if (a['stage'] in (31,32,33,34,50,51) and
                all(a[k] == b[k] for k in INT_FIELDS) and a['words'][:-1] == b['words'][:-1]
                and a['words'][-1] != b['words'][-1] and not (a['stage'] == 32 and a['flags'] == 2)):
            candidate = dict(index=index, left=a, right=b,
                             boundary='curve_quotient_expression' if a['stage'] == 34 else 'coarse_function_call',
                             arithmetic_cause_proven=False)
            if scalar_operation is None: scalar_operation = dict(candidate,complete_inputs_equal=False)
            context_equal = (a['stage'] in (33,34,50) or
                             a['stage'] in (31,32) and a['slot'] in equal_constants and
                             (a['stage'] != 32 or a['kind'] != 2 or 18 in equal_resources) or
                             a['stage'] == 51 and a['flags'] == 4 and a['slot'] in equal_constants and 19 in equal_resources)
            if equal_operation is None and context_equal:
                equal_operation = dict(candidate,complete_inputs_equal=True)
    return dict(earliest_difference=earliest, earliest_equal_input_operation=equal_operation,
                earliest_equal_explicit_scalar_operation=scalar_operation,
                records=len(left), arithmetic_cause_proven=False)


def validate_producer_source_phase(descriptor, current_sources, directory, producer_tool, read):
    """An explicit old analyzer snapshot cannot change any captured hooks or DSP."""
    tool_name = 'tools/saturation_serial_observer.py'
    producer_sources = dict(current_sources)
    phase = dict(producer_tool=descriptor['sources'][tool_name], analyzer_tool=current_sources[tool_name],
                 historical_producer=producer_tool is not None, generator_and_hooks_unchanged=True)
    if producer_tool is not None:
        old = read(Path(producer_tool),1048576)
        require(revision.identity(old) == descriptor['sources'][tool_name],
                'producer tool must match compiled descriptor identity')
        current = read(Path(__file__).resolve(),1048576)
        require(revision.identity(current) == current_sources[tool_name], 'current analyzer identity required')
        old_functions = {n.name:n for n in ast.parse(old).body if isinstance(n,ast.FunctionDef)}
        current_functions = {n.name:n for n in ast.parse(current).body if isinstance(n,ast.FunctionDef)}
        for name in ('recover','insertions','generate_candidate','generate_driver','generate_directory','source_inventory'):
            require(name in old_functions and name in current_functions and
                    ast.dump(old_functions[name]) == ast.dump(current_functions[name]),
                    'producer generator/hook functions must be unchanged: '+name)
        for original,observed,generate in (('Candidate.original.cpp','SaturationCandidateObserved.cpp',generate_candidate),
                                           ('Fixture.original.cpp','SaturationFixtureObserved.cpp',generate_driver)):
            require(generate(read(directory/original,1048576)) == read(directory/observed,1048576),
                    'current generated payload must equal captured producer bytes')
        producer_sources[tool_name] = revision.identity(old)
    require(descriptor['sources'] == producer_sources, 'all producer sources except explicit analyzer phase must match')
    return phase


def analyze(release_capture, debug_capture, output, producer_tool=None):
    output = fresh(output)
    protected = source_inventory()
    bindings = {}

    def read(path, limit):
        path = Path(path)
        raw = revision.read_regular(path, limit)
        bindings[str(path)] = revision.identity(raw)
        return raw

    traces, qualifications, phases = {}, {}, {}
    for profile, directory in (('Release',release_capture), ('Debug',debug_capture)):
        directory = Path(directory)
        revision.plain_path(directory)
        receipt = revision.json_bytes(read(directory/'capture.json', 65536))
        names = {f'serial-characterization.partition-{p}.actual.f64' for p in PARTITIONS} | {'trace.jsonl'}
        revision.members(directory, names | {'capture.json'})
        require(set(receipt) == {'schema','profile','source_descriptor_sha256','files'} and
                type(receipt['schema']) is int and receipt['schema'] == 1 and receipt['profile'] == profile
                and set(receipt['files']) == names, 'exact capture receipt schema/profile required')
        for name,bound in receipt['files'].items():
            require(revision.identity(read(directory/name, TRACE_LIMIT)) == bound, 'capture payload identity mismatch')
        descriptor_path = ROOT/'build/windows-debug/saturation-serial-observer/source.json'
        descriptor_raw = read(descriptor_path, 1048576)
        require(revision.identity(descriptor_raw)['sha256'] == receipt['source_descriptor_sha256'], 'compiled source descriptor identity required')
        require(read(descriptor_path.parent/'SaturationSerialSource.h',1024) ==
                ('#define RACKSYNTH_SERIAL_SOURCE_SHA256 "'+receipt['source_descriptor_sha256']+'"\n').encode(),
                'compiled descriptor binding required')
        descriptor = revision.json_bytes(descriptor_raw)
        phases[profile] = validate_producer_source_phase(descriptor,protected,descriptor_path.parent,producer_tool,read)
        require(descriptor['candidate_recovered_exact'] is True
                and descriptor['fixture_recovered_exact'] is True, 'source protection/recovery required')
        for name,bound in descriptor['generated'].items():
            require(revision.identity(read(descriptor_path.parent/name, 1048576)) == bound, 'generated source identity mismatch')
        for stem,observed in (('Candidate','SaturationCandidateObserved.cpp'),('Fixture','SaturationFixtureObserved.cpp')):
            require(recover(read(descriptor_path.parent/observed,1048576)) ==
                    read(descriptor_path.parent/(stem+'.original.cpp'),1048576) ==
                    read(descriptor_path.parent/(stem+'.recovered.cpp'),1048576), 'original/generated/recovered source mismatch')
        attempt, pin = SAVED[profile]
        archive = ROOT/'build/saturation-fixture-revision-20261023/task2'/profile/'capture'/attempt/'native'
        saved, actual = {}, {}
        for p in PARTITIONS:
            name = f'serial-characterization.partition-{p}.actual.f64'
            saved[p], actual[p] = read(archive/name,32768), read(directory/name,32768)
            require(revision.identity(saved[p])['sha256'] == pin, 'archived profile waveform pin mismatch')
        qualifications[profile] = qualify_partitions(actual,saved)
        traces[profile] = read_trace(directory/'trace.jsonl')
    result = dict(schema=1, status='QUALIFIED', qualification=qualifications,
                  comparison=compare_traces(traces['Release'],traces['Debug']), consumed=bindings, source_phases=phases,
                  CPU='DEFERRED', selected_factor=None, adoptable=False, production='FORCED_BYPASS')
    require(source_inventory() == protected, 'protected inputs changed during analysis')
    for path,bound in bindings.items():
        require(revision.identity(revision.read_regular(Path(path),TRACE_LIMIT)) == bound,
                'consumed evidence changed during analysis')
    output.parent.mkdir(parents=True,exist_ok=True)
    with output.open('x',encoding='utf-8') as stream:
        json.dump(result, stream, indent=2); stream.write('\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument('--generate', type=Path)
    actions.add_argument('--analyze', nargs=2, type=Path, metavar=('RELEASE_CAPTURE','DEBUG_CAPTURE'))
    parser.add_argument('--output',type=Path)
    parser.add_argument('--producer-tool',type=Path)
    args = parser.parse_args()
    try:
        if args.generate:
            require(args.output is None and args.producer_tool is None, '--output/--producer-tool are only for analysis')
            generate_directory(args.generate)
        else:
            require(args.output is not None, 'analysis --output required')
            analyze(*args.analyze,args.output,producer_tool=args.producer_tool)
        return 0
    except (ValueError,OSError,KeyError) as error:
        parser.exit(1, 'SERIAL_OBSERVER_REFUSAL '+str(error)+'\n')


if __name__ == '__main__':
    raise SystemExit(main())
