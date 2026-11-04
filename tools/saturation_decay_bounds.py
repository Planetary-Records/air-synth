"""Coefficient-only native export and independent, outward-rounded history bounds.

No audio is rendered here. Bounds describe real recurrences with captured binary64
coefficients; rounding and upstream differences remain separate forcing terms.
"""
import argparse
import array
from decimal import Decimal, localcontext, ROUND_CEILING, ROUND_FLOOR
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PINNED = '8b7f69ca9696094791df56421b87b9c0bf338d2f0dd1762f81c51e21a7f3737f'
BEGIN = b'\n/* AIRACK_DECAY_EXPORT_BEGIN */\n'
END = b'\n/* AIRACK_DECAY_EXPORT_END */\n'
PRECISION = 90


def require(ok, message):
    if not ok:
        raise ValueError(message)


def identity(path):
    raw = Path(path).read_bytes()
    return dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def verify_identity(path, expected):
    require(identity(path) == expected, 'source/input/image identity mismatch: ' + str(path))


def number(x):
    v = Decimal.from_float(x) if isinstance(x, float) else Decimal(x)
    require(v.is_finite(), 'nonfinite coefficient')
    return v


def sqrt_interval(x):
    """Decimal square-root bracket, certified by exact rational inequalities."""
    with localcontext() as ctx:
        ctx.prec = PRECISION
        root = x.sqrt()
        lo, hi = root.next_minus(), root.next_plus()
    require(Fraction(lo) ** 2 <= Fraction(x) <= Fraction(hi) ** 2, 'sqrt bracket')
    return lo, hi


def power_up(x, n):
    require(isinstance(n, int) and n >= 0 and 0 <= x <= 1, 'decay exponent/domain')
    with localcontext() as ctx:
        ctx.prec = PRECISION
        ctx.rounding = ROUND_CEILING
        result = Decimal(1)
        while n:
            if n & 1:
                result *= x
            n //= 2
            if n:
                x *= x
        return result


def biquad(coefficients):
    require(len(coefficients) == 5, 'five normalized biquad coefficients required')
    b0, b1, b2, a1, a2 = map(number, coefficients)
    # Exact coefficient arithmetic determines the pole family, never a float test.
    discriminant = 4 * Fraction(a2) - Fraction(a1) ** 2
    require(0 < a2 < 1 and discriminant > 0,
            'unsupported real/repeated or unstable poles; complex stable pair required')
    with localcontext() as ctx:
        ctx.prec = PRECISION
        ctx.rounding = ROUND_CEILING
        # Convert D downwards before bracketing its positive square root.
        with localcontext() as down:
            down.rounding = ROUND_FLOOR
            d = Decimal(discriminant.numerator) / Decimal(discriminant.denominator)
        d_lo, _ = sqrt_interval(d)
        r_lo, r = sqrt_interval(a2)
        require(r < 1 and d_lo > 0, 'unresolved outward pole enclosure')
        k = 1 + 2 * max(abs(a1) / 2 + 1, abs(a2) + abs(a1) / 2) / d_lo
        # Impulse envelope exploits B cancellation, rather than ||A^n|| ||B||.
        bf = [Fraction(b1) - Fraction(a1) * Fraction(b0), Fraction(b2) - Fraction(a2) * Fraction(b0)]
        mb = -Fraction(a1) * bf[0] / 2 + bf[1]
        def up(v):
            return Decimal(v.numerator) / Decimal(v.denominator)
        impulse = up(abs(bf[0])) + 2 * up(abs(mb)) / d_lo
        return dict(coefficients=[b0, b1, b2, a1, a2], radius_lower=r_lo, radius_upper=r,
                    stability_margin_lower=1-r, state_constant=k, impulse_constant=impulse,
                    l1_output=abs(b0) + impulse / (1-r))


def homogeneous(bound, n, initial):
    require(number(initial) >= 0, 'nonnegative initial norm required')
    with localcontext() as ctx:
        ctx.prec = PRECISION
        ctx.rounding = ROUND_CEILING
        return bound['state_constant'] * number(initial) * power_up(bound['radius_upper'], n)


def cascade(coefficients, n, initial):
    """Every section starts with ||delta z||inf<=initial; input difference is zero.

    q-weighted convolution accounts for upstream initial state driving all later
    sections. L(q)=|b0|+C_B/(q-r), not radius-only multiplication.
    """
    require(coefficients and number(initial) >= 0, 'empty path or negative initial bound')
    bounds = [biquad(c) for c in coefficients]
    with localcontext() as ctx:
        ctx.prec = PRECISION
        ctx.rounding = ROUND_CEILING
        r = max(b['radius_upper'] for b in bounds)
        q = r + (1-r) / 16
        total, downstream = Decimal(0), Decimal(1)
        for b in reversed(bounds):
            total += b['state_constant'] * number(initial) * downstream
            downstream *= abs(b['coefficients'][0]) + b['impulse_constant'] / (q-b['radius_upper'])
        return dict(q=q, path_constant_per_initial=total/number(initial) if initial else Decimal(0),
                    output_bound=total*power_up(q, n), weighted_input_gain=downstream)


def resampler4(resources, n, initial, q_native=None):
    """Two stages, actual once/twice-native phase clocks and held-odd down path."""
    with localcontext() as ctx:
        ctx.prec = PRECISION
        ctx.rounding = ROUND_CEILING
        values = [number(c) for cs in resources.values() for c in cs]
        require(values and all(abs(c) < 1 for c in values), 'unstable allpass')
        r = max(map(abs, values))
        if q_native is None:
            q = r + (1-r)/16
            phase = (1+q)/2
            q_native = phase*phase
        q = number(q_native)
        phase, _ = sqrt_interval(q)
        require(r < phase < 1 and r < q < 1, 'allpass envelope domain')
        def chain(stage, direction, path, envelope):
            cs = [number(x) for x in resources[f'iir{stage}-{direction}-{path}']]
            constant, transfer = Decimal(0), Decimal(1)
            for c in reversed(cs):
                constant += transfer
                # 1-c^2 is rounded UP before summing the positive series.
                cf = Fraction(c)
                numerator = Decimal((1-cf*cf).numerator)/Decimal((1-cf*cf).denominator)
                transfer *= abs(c) + numerator/(envelope-abs(c))
            return constant, transfer
        def stage(s, direction, envelope):
            a, ga = chain(s, direction, 'direct', envelope)
            b, gb = chain(s, direction, 'delayed', envelope)
            if direction == 'up':
                return max(a,b), max(ga,gb)
            # Independent held value contributes only at the first update.
            return (a+b/envelope+1)/2, (ga+gb/envelope)/2
        u1, gu1 = stage(1,'up',q)
        u2, gu2 = stage(2,'up',phase)
        d1, gd1 = stage(1,'down',q)
        d2, gd2 = stage(2,'down',phase)
        up_constant = u2+u1*gu2/q
        down_constant = d1+d2*gd1
        return dict(q_native=q, q_phase_lower=phase, up_constant=up_constant, down_constant=down_constant,
                    up_bound=number(initial)*up_constant*power_up(q,n),
                    down_bound=number(initial)*down_constant*power_up(q,n),
                    up_input_gain=gu1*gu2/q, down_input_gain=gd1*gd2/q)


def validate_capture(capture, cases):
    require(capture.get('schema') == 1 and capture.get('role') == 'coefficient-only-original-prepare', 'capture role/schema')
    expected = {(f,r,c,h,p) for f,r,c in cases for h in range(3 if c.startswith('mode-') else 2) for p in range(3)}
    rows, profiles = capture.get('rows', []), {}
    keys = [(x['factor'],x['rate'],x['case'],x['history'],x['phase']) for x in rows]
    require(len(keys) == len(set(keys)) and set(keys) == expected, 'all retained histories/phases must be prepared exactly once')
    for row in rows:
        s = row['settings']
        require([float.fromhex(x) for x in s['values']] == [180,3500,600,1200,0,0,1]
                and s['drive_compensation'] == 0 and len(s['lanes']) == 4, 'unexpected prepared continuous settings')
        for lane in s['lanes']:
            require(lane[0] in (0,1,2) and lane[1] == 0 and [float.fromhex(x) for x in lane[2:]] == [18,0,1,0], 'unexpected lane settings')
        require(set(row['coefficients']) == {'lowLP','lowHP','highLP','highHP','highAP','pre','post','shelfLow','shelfHigh'}, 'coefficient fields')
        for name, values in row['coefficients'].items():
            values = [float.fromhex(x) for x in values]
            if name.startswith('shelf'):
                require(values == [1,0,0,0,0], 'retained zero tilt must be identity')
            else:
                biquad(values)
        require(len(row['recurrences']) == 3 and all(0 < float.fromhex(x) < 1 for x in row['recurrences']), 'lane recurrence values')
        key = (row['factor'],row['rate'])
        fixed = {k: row[k] for k in ('coefficients','recurrences','drive_gain','R')}
        require(key not in profiles or profiles[key] == fixed, 'additional coefficient set found; expand analysis')
        profiles[key] = fixed
    require(len(profiles) == 6, 'six factor/rate sets required')
    expected_resources = {f'iir{s}-{d}-{p}' for s in (1,2) for d in ('up','down') for p in ('direct','delayed')}
    expected_resources |= {f'reference-fir{s}' for s in range(1,7)}
    require(set(capture['resources']) == expected_resources, 'complete used allpass/FIR resources required')
    for name, values in capture['resources'].items():
        require(values and all(math.isfinite(float.fromhex(x)) for x in values), 'invalid resource values')
        if name.startswith('iir'):
            require(all(abs(float.fromhex(x)) < 1 for x in values), 'unstable allpass resource')
    return profiles


def first_order(alpha, n, initial, forcing=Decimal(0)):
    a, z, f = map(number, (alpha, initial, forcing))
    require(0 <= a < 1 and z >= 0 and f >= 0, 'stable first-order nonnegative bounds required')
    with localcontext() as ctx:
        ctx.prec = PRECISION
        ctx.rounding = ROUND_CEILING
        # Infinite forcing sum is deliberately conservative at every finite n.
        return dict(homogeneous=z*power_up(a, n), forcing=f/(1-a), radius=a,
                    stability_margin=1-a)


def envelope_release_at_observation(alpha, observation, factor, common_start):
    frame, oversample, _ = observation['where']
    require(frame >= common_start and 0 <= oversample < factor, 'observation outside common-input clock')
    # The observer reads envelope after this oversample's update.
    updates = (frame-common_start)*factor + oversample + 1
    release = first_order(alpha, updates, Decimal(1))['homogeneous']
    with localcontext() as ctx:
        ctx.prec = PRECISION
        ctx.rounding = ROUND_CEILING
        return dict(observed_at=observation['where'], common_start_frame=common_start,
                    elapsed_high_rate_updates=updates,
                    update_convention='zero-based [frame, oversample, channel]; envelope after current update; initial state before common-start frame/oversample 0',
                    unit_release_at_observation=release,
                    hypothetical_initial_difference_for_observed=number(observation['max'])/release)


def recover(generated):
    return re.sub(re.escape(BEGIN) + b'.*?' + re.escape(END), b'', generated, flags=re.DOTALL)


EXPORT = r'''
namespace decay_capture {
std::ostream* output = nullptr;
std::string caseId;
unsigned phase = 0, history = 0;
bool first = true;
void hex(double v) { *output << '"' << std::hexfloat << v << '"'; }
void coefficients(const char* name, const Coefficients& c) {
    *output << '"' << name << "\":[";
    const double values[] { c.b0, c.b1, c.b2, c.a1, c.a2 };
    for (unsigned n = 0; n < 5; ++n) { if (n) *output << ','; hex(values[n]); }
    *output << ']';
}
void emit(const Endpoint& e) {
    if (!output) return;
    *output << (first ? "" : ",") << "{\"factor\":" << e.factor << ",\"rate\":" << unsigned(e.rate)
        << ",\"case\":\"" << caseId << "\",\"phase\":" << phase << ",\"history\":" << history
        << ",\"settings\":{\"mode\":" << unsigned(e.current.mode)
        << ",\"pre_enabled\":" << e.current.preLowCutEnabled << ",\"post_enabled\":" << e.current.postHighCutEnabled
        << ",\"drive_compensation\":" << e.current.driveCompensation << ",\"values\":[";
    const double settings[] { e.current.lowCrossoverHz, e.current.highCrossoverHz, e.current.preLowCutHz,
        e.current.postHighCutHz, e.current.inputTrimDb, e.current.outputTrimDb, e.current.mix };
    for (unsigned n = 0; n < 7; ++n) { if (n) *output << ','; hex(settings[n]); }
    *output << "],\"lanes\":[";
    for (unsigned n = 0; n < 4; ++n) {
        if (n) *output << ',';
        const auto& l = e.current.lanes[n]; *output << '[' << unsigned(l.character) << ',' << l.bypassed << ',';
        hex(l.driveDb); *output << ','; hex(l.preTiltDb); *output << ','; hex(l.mix); *output << ','; hex(l.trimDb); *output << ']';
    }
    *output << "]},\"coefficients\":{";
    coefficients("lowLP", e.network.lowLP); *output << ','; coefficients("lowHP", e.network.lowHP); *output << ',';
    coefficients("highLP", e.network.highLP); *output << ','; coefficients("highHP", e.network.highHP); *output << ',';
    coefficients("highAP", e.network.highAP); *output << ','; coefficients("pre", e.pre); *output << ',';
    coefficients("post", e.post); *output << ','; coefficients("shelfLow", e.shelfLow[0]); *output << ',';
    coefficients("shelfHigh", e.shelfHigh[0]);
    *output << "},\"recurrences\":["; hex(e.dcAlpha); *output << ','; hex(e.sagAttack); *output << ','; hex(e.sagRelease);
    *output << "],\"drive_gain\":"; hex(e.driveGain[0]); *output << ",\"R\":"; hex(R);
    *output << ",\"wide_network\":" << e.useWideNetwork << '}'; first = false;
}
}
'''


def generate(original, cases):
    require(hashlib.sha256(original).hexdigest() == PINNED, 'original Candidate identity')
    require(cases and len(set(cases)) == len(cases), 'unique retained cases required')
    newline = b'\r\n' if b'\r\n' in original else b'\n'
    edits = []
    def insert(anchor, code):
        needle = anchor.encode().replace(b'\n', newline)
        require(original.count(needle) == 1, 'unique coefficient-only export anchor')
        at = original.index(needle) + len(needle)
        edits.append((at, BEGIN + code.encode() + END))
    insert('#include <vector>', '#include <fstream>\n#include <filesystem>\n#include <iomanip>\n#include <iostream>')
    insert('static_assert(sizeof(Candidate::Frame) == 16);', EXPORT)
    insert('            endpoint.clear();', '\nif (&endpoint == &replacement->endpoints[0]) decay_capture::emit(endpoint);')
    rows = ',\n'.join('{' + f'{f},{r},"{c}"' + '}' for f, r, c in cases)
    main = r'''
#include "SaturationHistoryCases.h"
int main(int argc, char** argv) {
    using namespace airack::probe;
    if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
    std::ofstream file(argv[1]); file.exceptions(std::ios::badbit | std::ios::failbit);
    decay_capture::output = &file;
    file << "{\"schema\":1,\"role\":\"coefficient-only-original-prepare\",\"rows\":[";
    struct Row { unsigned factor, rate; const char* id; };
    const Row selected[] { ROWS };
    for (const auto& row : selected) {
        bool found = false;
        for (bool modes : { false, true }) for (const auto& c : history::cases(modes)) if (c.id == row.id) {
            found = true; decay_capture::caseId = c.id;
            for (unsigned h = 0; h < (modes ? 3u : 2u); ++h) for (unsigned phase = 0; phase < 3; ++phase) {
                decay_capture::history = h; decay_capture::phase = phase;
                Candidate candidate;
                if (!candidate.setSettings(c.settings(phase,h)) || !candidate.prepare(row.rate,row.factor)) return 3;
            }
        }
        if (!found) return 4;
    }
    file << "],\"resources\":{";
    bool first = true;
    for (const auto& entry : saturationCoefficientManifest()) {
        const std::string name(entry.name);
        if (!(name.starts_with("iir1-") || name.starts_with("iir2-") || name.starts_with("reference-fir"))) continue;
        file << (first ? "" : ",") << '"' << name << "\":[";
        for (std::size_t n = 0; n < entry.values.size(); ++n) { if (n) file << ','; decay_capture::hex(entry.values[n]); }
        file << ']'; first = false;
    }
    file << "}}\n"; file.close(); decay_capture::output = nullptr;
    std::cout << "COEFFICIENT_EXPORT_COMPLETE\n";
}
'''.replace('ROWS', rows)
    edits.append((len(original), BEGIN + main.encode() + END))
    generated = original
    for at, code in sorted(edits, reverse=True):
        generated = generated[:at] + code + generated[at:]
    require(recover(generated) == original, 'byte-exact original recovery')
    return generated


def analyze(capture_path, receipt_path, output):
    """Fail-closed provenance; emit a compact ledger, never an acceptance packet."""
    receipt = json.loads(receipt_path.read_text())
    proof = receipt_path.parent/'files'
    for rel, expected in receipt.items():
        verify_identity(proof/rel, expected)
    rel_capture = capture_path.relative_to(ROOT).as_posix()
    require(rel_capture in receipt, 'unbound native coefficient capture')
    verify_identity(capture_path, receipt[rel_capture])
    require(receipt['probes/saturation/SaturationCandidate.cpp']['sha256'] == PINNED, 'original design source binding')
    packet_path = ROOT/'docs/verification/saturation-history-diagnosis-20261021/task2-stage-diagnosis.json'
    prior = json.loads(packet_path.read_text())
    preimage_path = ROOT/'build/saturation-decay-bounds-20261022/preimage-01/identities.json'
    preimage = json.loads(preimage_path.read_text())
    verify_identity(packet_path, preimage[packet_path.relative_to(ROOT).as_posix()])
    cases = [(c['factor'],c['rate'],c['case']) for c in prior['cases']]
    capture = json.loads(capture_path.read_text())
    profiles = validate_capture(capture,cases)
    initial = Decimal(1 << 1025)  # Strictly exceeds 2*DBL_MAX; no guessed small histories.
    ledger = dict(schema=1, role='diagnostic-decay-bounds-not-acceptance', precision=PRECISION,
                  rounding='positive operations ROUND_CEILING; exact-rational square-root certificates',
                  original_acceptance=prior['original_acceptance'], cpu='DEFERRED', timing='NOT_RUN',
                  heap='NOT_RUN', cpu_output_integrity='NOT_RUN', selected_factor=None, adoptable=False,
                  production='FORCED_BYPASS', open_gates=['R4','R5','R6','R7'],
                  provenance=dict(capture=str(capture_path), capture_identity=identity(capture_path),
                      receipt=str(receipt_path), receipt_identity=identity(receipt_path),
                      preimage_inventory=str(preimage_path), preimage_identity=identity(preimage_path),
                      prior_packet=str(packet_path), prior_packet_identity=identity(packet_path)),
                  assumptions=dict(initial_state_difference_bound=initial,
                      initial_bound_basis='any two finite binary64 state cells differ by <2^1025; conditional on finite state, no state equality assumed',
                      input_difference_for_homogeneous_claim=0, native_stimulus_absolute_bound='.05',
                      binary64_roundoff='separate per-operation additive forcing, not included in homogeneous numbers',
                      nonlinear='no global binary64 nonlinear contraction claimed; prior exact quotient witnesses retained'),
                  coefficients=[], cases=[], resources=capture['resources'],
                  preparations=[dict(factor=r['factor'],rate=r['rate'],case=r['case'],phase=r['phase'],
                      history=r['history'],mode=r['settings']['mode'],pre_enabled=r['settings']['pre_enabled'],
                      post_enabled=r['settings']['post_enabled'],characters=[l[0] for l in r['settings']['lanes']])
                      for r in capture['rows']])
    with localcontext() as ctx:
        ctx.prec = PRECISION
        ctx.rounding = ROUND_CEILING
        fir = [[number(float.fromhex(x)) for x in capture['resources'][f'reference-fir{s}']] for s in range(1,7)]
        require([len(cs) for cs in fir] == [257,129,65,33,17,17], 'original FIR lengths')
        fir_up_gain, fir_down_gain = Decimal(1), Decimal(1)
        for cs in fir:
            fir_up_gain *= 2*max(sum(map(abs,cs[::2])),sum(map(abs,cs[1::2])))
            fir_down_gain *= sum(map(abs,cs))
        allpass = {k:[float.fromhex(x) for x in v] for k,v in capture['resources'].items() if k.startswith('iir')}
        cohort_results = {}
        for (factor,rate), profile in sorted(profiles.items()):
            coefficients = {k:[float.fromhex(x) for x in v] for k,v in profile['coefficients'].items()}
            linear = {k:biquad(c) for k,c in coefficients.items() if not k.startswith('shelf')}
            qh = max(b['radius_upper'] for k,b in linear.items() if k != 'pre')
            qh += (1-qh)/16
            qn = power_up(qh,factor)
            paths = {'low':['lowLP','lowLP','highAP'], 'mid':['lowHP','lowHP','highLP','highLP'],
                     'high':['lowHP','lowHP','highHP','highHP']}
            def path_constants(names):
                c, g = Decimal(0), Decimal(1)
                for name in reversed(names):
                    b = linear[name]
                    c += b['state_constant']*g
                    g *= abs(b['coefficients'][0])+b['impulse_constant']/(qh-b['radius_upper'])
                return c,g
            pre = linear['pre']
            require(qn > pre['radius_upper'], 'native pre-cut clock envelope')
            pre_constant = pre['state_constant']
            if factor == 4:
                resampling = resampler4(allpass,0,Decimal(1),qn)
                up_constant, up_gain = resampling['up_constant'], resampling['up_input_gain']
            else:
                # Each FIR cell's initial content is gone by native frame171.
                # Sum six independent FIR initial-history injections, including
                # downstream gains, rather than treating all cells as one bank.
                # Invert using a LOWER denominator, not an outward upper power.
                with localcontext() as down:
                    down.rounding = ROUND_FLOOR
                    delay_lower = qn ** 172
                require(delay_lower > 0, 'FIR delay envelope')
                up_constant = 6*fir_up_gain/delay_lower
                up_gain = fir_up_gain/delay_lower
                resampling = dict(fir_lengths=[257,129,65,33,17,17], native_support='683/4',
                                  first_untainted_native_frame=171, conservative_analysis_frame=172,
                                  support_scope='initial FIR cells only; upstream IIR/nonlinear state excluded',
                                  up_l1=fir_up_gain, down_l1=fir_down_gain,
                                  up_weighted_input_gain=up_gain, down_homogeneous_after171=0)
            front = up_constant + pre_constant*up_gain
            result = dict(factor=factor,rate=rate, high_rate=factor*rate,
                          binary64_hex=profile, biquads=linear, q_high=qh,q_native=qn,
                          resampling=resampling, paths={}, windows={}, lane_recurrences={})
            for name,names in paths.items():
                c,g = path_constants(names)
                result['paths'][name] = dict(sections=names, network_initial_constant=c,
                    weighted_network_input_gain=g, front_initial_constant=c+front*g/qn,
                    front_scope='native pre-cut + wet/dry upsampler + crossover; includes pre even if bypassed, excludes nonlinear lane/post/down')
            fade = math.ceil(.020*rate)
            # Start, first fade frame, last fade frame, last entire fade+128 frame.
            for label, native_n in [('common2s',2*rate),('prewindow_start',2*rate-64),
                                    ('last_fade',2*rate+fade-1),('last_return',2*rate+fade+127)]:
                high_n = native_n*factor
                w = dict(native_updates=native_n,high_updates=high_n,
                         pre_initial=homogeneous(pre,native_n,initial),
                         post_initial=homogeneous(linear['post'],high_n,initial), network={},full_linear_front={})
                for name,p in result['paths'].items():
                    w['network'][name] = p['network_initial_constant']*initial*power_up(qh,high_n)
                    w['full_linear_front'][name] = p['front_initial_constant']*initial*power_up(qn,native_n)
                if factor == 4:
                    w['resampling'] = resampler4(allpass,native_n,initial,qn)
                else:
                    w['resampling'] = dict(initial_up=0,initial_down=0)
                result['windows'][label] = w
            for i,name in enumerate(('dc','sag_attack','sag_release')):
                alpha = float.fromhex(profile['recurrences'][i])
                result['lane_recurrences'][name] = dict(hex=profile['recurrences'][i],
                    after2s_per_unit=first_order(alpha,2*rate*factor,Decimal(1)),
                    after2s_arbitrary_finite=first_order(alpha,2*rate*factor,initial),
                    after_return_per_unit=first_order(alpha,(2*rate+fade+127)*factor,Decimal(1)),
                    return_only_per_unit=first_order(alpha,(fade+128)*factor,Decimal(1)))
            cohort_results[f'{factor}/{rate}'] = result
            ledger['coefficients'].append(result)
        for case in prior['cases']:
            raw = Path(case['complete_trace_directory'])
            # Current packets point to complete native trace directories.
            if raw.name != 'native':
                raw = raw/'native'
            observations = {}
            common_input, input_peak = None, 0.
            for name, expected in case['input_files'].items():
                path = Path(case['input'])/name
                verify_identity(path,expected)
                if name.endswith('.input.f64'):
                    raw_input = path.read_bytes()
                    require(len(raw_input) % 16 == 0, 'native stereo input extent')
                    samples = array.array('d'); samples.frombytes(raw_input)
                    require(all(math.isfinite(x) for x in samples), 'nonfinite retained input')
                    input_peak = max(input_peak,max(map(abs,samples)))
                    common = raw_input[4096*16:]
                    require(common_input is None or common == common_input, 'native common interval is not bit identical')
                    common_input = common
            require(common_input and input_peak <= .05, 'retained native input bound')
            for h in case['stages']:
                path = raw/f"{case['case']}.{h}.output.f64.stages.json"
                verify_identity(path,preimage[path.relative_to(ROOT).as_posix()])
                metrics = json.loads(path.read_text())['metrics']
                observations[h] = {k:{a:v[a] for a in ('max','nonzero','first','where')}
                    for k,v in metrics.items() if any(k.endswith('/'+field) for field in
                        ('input','up_wet','pre_z1','pre_filtered','post_before','post_filtered','x','u','shaped','dc','envelope'))}
            entry = dict(factor=case['factor'],rate=case['rate'],case=case['case'],
                coefficient_set=f"{case['factor']}/{case['rate']}", original=case['original'],
                diagnosis=case['diagnosis_category'], observations=observations,
                qualification=case['qualified'], original_command=case['command'], original_exit=case['exit'],
                original_image=case['image'], input=case['input'], input_files=case['input_files'],
                output_and_trace_identities=case['output_and_trace_identities'],
                bounded_input=dict(absolute_peak=input_peak, common_after_frame=4096, byte_identical=True),
                status='PASS_CONTROL' if case['original']['passed'] else 'FAIL_OPEN',
                limit='homogeneous bounds do not remove upstream, rounding, sag-envelope or nonlinear forcing')
            if case['case'] == 'post-1-2' and case['factor'] == 64:
                envelope = observations['old1']['0/0/lane1/envelope']
                recurrence = cohort_results[entry['coefficient_set']]['lane_recurrences']['sag_release']
                release = recurrence['after2s_per_unit']['homogeneous']
                entry['dense_envelope'] = dict(observed_pre_window_difference=envelope['max'],
                    unit_release_after2s=release,
                    interpretation='conditional plausibility scale, not measured common-start state or causal allocation')
                if envelope['max']:
                    entry['dense_envelope'].update(envelope_release_at_observation(
                        float.fromhex(recurrence['hex']), envelope, case['factor'], entry['bounded_input']['common_after_frame']))
                else:
                    entry['dense_envelope']['hypothetical_initial_difference_for_observed'] = number(envelope['max'])/release
            ledger['cases'].append(entry)
    require(sum(c['status']=='FAIL_OPEN' for c in ledger['cases']) == 23, 'all original failures must remain open')
    output.parent.mkdir(parents=True,exist_ok=True)
    require(not output.exists(), 'fresh ledger output required')
    output.write_text(json.dumps(ledger,indent=2,default=str)+'\n')
    print('DECAY_LEDGER_COMPLETE',len(profiles),'cohorts',len(cases),'cases; 23 FAIL_OPEN')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generate', type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--analyze', type=Path)
    parser.add_argument('--receipt', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.analyze:
        require(args.generate is None and args.source is None and args.receipt and args.output, 'analyze requires receipt/fresh output')
        analyze(args.analyze,args.receipt,args.output)
        return
    require(args.generate is not None and not args.receipt and not args.output, 'select --generate or --analyze')
    packet = ROOT/'docs/verification/saturation-history-diagnosis-20261021/task2-stage-diagnosis.json'
    cases = [(c['factor'], c['rate'], c['case']) for c in json.loads(packet.read_text())['cases']]
    import saturation_numerical_stability as stability
    source = args.source or stability.FROZEN / 'SaturationCandidate.cpp'
    require(source.name == 'SaturationCandidate.cpp', 'historical source filename')
    stability.check_frozen(source.parent)
    original = source.read_bytes()
    generated = generate(original, cases)
    args.generate.parent.mkdir(parents=True, exist_ok=True)
    args.generate.write_bytes(generated)
    args.generate.with_suffix('.recovered.cpp').write_bytes(recover(generated))
    args.generate.with_suffix('.recovery.json').write_text(json.dumps(dict(original_sha256=PINNED,
        generated=identity(args.generate), recovered_sha256=hashlib.sha256(recover(generated)).hexdigest(),
        insertions=4, case_packet=identity(packet), audio_process_calls=0), indent=2)+'\n')


if __name__ == '__main__':
    main()
