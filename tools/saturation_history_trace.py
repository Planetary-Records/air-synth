"""Private read-only history trace analysis, never a production audio oracle."""
import array
import argparse
from decimal import Decimal, localcontext
import hashlib
import json
import math
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = 'RackSynthSaturationHistoryTrace'
ACTION = 'return-phase2-readonly-trace'
PINNED = '8b7f69ca9696094791df56421b87b9c0bf338d2f0dd1762f81c51e21a7f3737f'
BEGIN = b'\n/* AIRACK_HISTORY_TRACE_BEGIN */\n'
END = b'\n/* AIRACK_HISTORY_TRACE_END */\n'


def recover(generated):
    return re.sub(re.escape(BEGIN) + b'.*?' + re.escape(END), b'', generated, flags=re.DOTALL)


def generate(original):
    require(hashlib.sha256(original).hexdigest() == PINNED, 'pinned original Candidate bytes')
    return insert_observer(original)


def insert_observer(original):
    """Read-only insertions shared by separately pinned historical/current actions."""
    newline = '\r\n' if b'\r\n' in original else '\n'
    edits = []

    def add(anchor, code, count=1):
        needle = anchor.replace('\n', newline).encode()
        require(original.count(needle) == count, 'unique observer anchor: ' + anchor[:80])
        at = 0
        for _ in range(count):
            at = original.index(needle, at) + len(needle)
            edits.append((at, BEGIN + code.encode() + END))

    add('#include <vector>', '#include "SaturationHistoryTrace.h"')
    add('        auto& s = *this;\n        s.advanceControls(); input[0] *= s.inputGain; input[1] *= s.inputGain;',
        'history_trace::endpoint(unsigned(s.current.mode), { &s.lanes[0], &s.lanes[1], &s.lanes[2], &s.lanes[3] }, input, s.preCut.z1, s.preCut.z2, s.postCut.z1, s.postCut.z2);')
    add('        const auto preFiltered = s.preCut.process(input, s.pre);',
        'history_trace::pre(preFiltered, s.preCut.z1, s.preCut.z2, s.current.preLowCutEnabled);')
    add('        std::array<std::array<wide_network::Quad, 3>, Capacity == 4 ? 4 : 0> wideBands;',
        'history_trace::up(wet.data(), dry.data(), unsigned(Capacity == 4 ? 4 : s.factor));')
    add('            const auto postFiltered = s.postCut.process(w, s.post);',
        'history_trace::post(unsigned(n), w, postFiltered, s.postCut.z1, s.postCut.z2, s.current.postHighCutEnabled);')
    quotient = 'shaped = (currentPrimitive - priorPrimitive) / delta;'
    require(original.count(quotient.encode()) == 3, 'three quotient paths')
    at = 0
    for occurrence in range(3):
        at = original.index(quotient.encode(), at) + len(quotient)
        character = 'Character' if occurrence < 2 else 's.character'
        code = f'history_trace::lane(this, unsigned(ch), unsigned({character}), x[ch], h[ch], u[ch], previousU[ch], delta, currentPrimitive, priorPrimitive, shaped, previousPrimitive[ch], unsigned(primitiveKind[ch]), unsigned(kind), unsigned(primitiveCharacter), previousX[ch], previousH[ch], previousDifference[ch], dc[ch], envelope);'
        edits.append((at, BEGIN + code.encode() + END))
    for character, count in (('Character', 2), ('s.character', 1)):
        anchor = f'shaped = cachedShape({character}, .5 * (u[ch] + previousU[ch])); primitiveKind[ch] = PrimitiveKind::none;'
        add(anchor, f'history_trace::lane(this, unsigned(ch), unsigned({character}), x[ch], h[ch], u[ch], previousU[ch], delta, 0, 0, shaped, previousPrimitive[ch], 0, 0, unsigned(primitiveCharacter), previousX[ch], previousH[ch], previousDifference[ch], dc[ch], envelope);', count)
    generated = original
    for at, code in sorted(edits, reverse=True):
        generated = generated[:at] + code + generated[at:]
    require(recover(generated) == original, 'byte-exact observer removal recovery')
    return generated


def require(ok, message):
    if not ok:
        raise ValueError(message)


def qualify(original, observed):
    require(original == observed and len(original) % 16 == 0, 'complete audio is not byte identical')
    return dict(byte_identical=True, frames=len(original) // 16,
                sha256=hashlib.sha256(original).hexdigest())


def validate_groups(factor, offsets, endpoints):
    require(factor in (4, 64) and endpoints in (1, 2) and offsets == list(range(factor * endpoints)),
            'trace group order/count/extent')


def pair_keys(left, right):
    require(left == right and len(set(left)) == len(left), 'trace stage pairing/order')
    return len(left)


def doubles(raw):
    require(len(raw) % 8 == 0, 'trace extent')
    values = array.array('d'); values.frombytes(raw)
    require(all(math.isfinite(x) for x in values), 'nonfinite trace')
    return values


def difference(left, right, start, end):
    a, b = doubles(left), doubles(right)
    require(len(a) == len(b) and 0 <= start < end <= len(a) // 2, 'paired extent')
    delta = [abs(a[n] - b[n]) for n in range(2 * start, 2 * end)]
    maximum = max(delta); index = delta.index(maximum)
    first = next((n for n, x in enumerate(delta) if x > 1e-12), None)
    return dict(max_error=maximum, maximum_frame=index // 2, maximum_channel=index % 2,
                first_frame=first // 2 if first is not None else None,
                first_channel=first % 2 if first is not None else None)


def analytic_quotient(character, current, previous, precision):
    """Independent scalar analysis, from exact binary64 arguments; no audio generation."""
    with localcontext() as context:
        context.prec = precision
        x, p = Decimal.from_float(current), Decimal.from_float(previous)
        require(x != p, 'nonzero scalar quotient extent')
        bias = Decimal.from_float(.35)
        b = math.tanh(.35)
        bias_shape, normalizer = Decimal.from_float(b), Decimal.from_float(1 - b * b)

        def logcosh(v):
            v = abs(v)
            return v + (1 + (-2 * v).exp()).ln() - Decimal(2).ln()

        def primitive(v):
            if character == 0:
                return logcosh(v)
            if character == 1:
                return (logcosh(v + bias) - bias_shape * v) / normalizer
            require(character == 2, 'known analytic character')
            v = abs(v)
            return v * v / 2 if v <= Decimal('.75') else (
                Decimal('.28125') + Decimal('.75') * (v - Decimal('.75')) +
                Decimal('.0625') * logcosh(4 * (v - Decimal('.75'))))

        return +(primitive(x) - primitive(p)) / (x - p)


def quotient_witness(left, right):
    a, b = ([float.fromhex(x) for x in values] for values in (left, right))
    require(len(a) == len(b) == 18 and a[12] == b[12], 'paired quotient record')
    result = dict(left=left, right=right, primitive_kind=[int(a[11]), int(b[11])],
                  actual_shaped_difference=abs(a[7] - b[7]))
    if a[11] != 1 or b[11] != 1:
        result['interpretation'] = 'MIDPOINT_OR_PREPARED_CURVE; not an exact-primitive roundoff claim'
        return result
    require(a[4] == a[2] - a[3] and b[4] == b[2] - b[3] and
            a[7] == (a[5] - a[6]) / a[4] and b[7] == (b[5] - b[6]) / b[4], 'actual quotient identity')
    q80 = [analytic_quotient(int(v[12]), v[2], v[3], 80) for v in (a, b)]
    q120 = [analytic_quotient(int(v[12]), v[2], v[3], 120) for v in (a, b)]
    with localcontext() as context:
        context.prec = 120
        convergence = max(abs(x - y) for x, y in zip(q80, q120))
        bound = abs(Decimal.from_float(a[2]) - Decimal.from_float(b[2])) + abs(
            Decimal.from_float(a[3]) - Decimal.from_float(b[3]))
        high_difference = abs(q120[0] - q120[1])
        require(convergence < Decimal('1e-60') and high_difference <= bound, 'converged analytic Lipschitz bound')
        result.update(analytic_q80=[str(x) for x in q80], analytic_q120=[str(x) for x in q120],
                      precision_convergence=str(convergence), analytic_difference=str(high_difference),
                      universal_L2_bound=str(bound), float_exceeds_L2_bound=Decimal.from_float(
                          result['actual_shaped_difference']) > bound,
                      amplification_over_bound=float(Decimal.from_float(result['actual_shaped_difference']) / bound)
                          if bound else None,
                      interpretation='EXACT_PRIMITIVE_QUOTIENT; scalar analysis only')
    return result


def depth_analysis(path, role, factor, fade):
    data = json.loads(path.read_text())
    # Mode returns can have different lane counts at each endpoint; exact native validation supplies the count.
    require(data['history'] == (1 if role == 'old1' else 2 if role == 'fresh' else 0) and
            data['window_frames'] == 64 + fade + 128, 'deep history/window binding')
    require(sum(data['branch_counts']) > 0 and data['cache_checks'] == sum(data['cache_used']) and
            data['primitive_checks'] == sum(data['branch_counts'][1:]), 'deep branch/cache counts')
    witnesses = {}
    for key, metric in data['metrics'].items():
        require(metric['samples'] > 0 and metric['nonzero'] <= metric['samples'], 'stage aggregate extent')
        if key.endswith('/shaped') and metric['max'] > 0:
            witnesses[key] = dict(where=metric['where'], **quotient_witness(metric['left'], metric['right']))
    return dict(raw=identity(path), counts={k: data[k] for k in ('branch_counts', 'cache_used', 'cache_checks',
                'cache_mismatch', 'primitive_checks', 'primitive_mismatch')}, metrics=data['metrics'], witnesses=witnesses)


def identity(path):
    raw = path.read_bytes()
    return dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def helpers():
    sys.path.insert(0, str(ROOT / 'tests'))
    import test_saturation_history_diagnosis as held
    return held


def endpoint_rows(path, total, factor, start, fade, source_change):
    values = doubles(path.read_bytes())
    require(len(values) == total * 11, 'complete endpoint trace extent')
    for frame in range(total):
        row = values[11 * frame:11 * (frame + 1)]
        event = (4096 if source_change and 4096 <= frame < 4096 + fade else
                 start if start <= frame < start + fade else None)
        count = 2 if event is not None else 1
        require(row[0] == frame and row[1] == count and
                row[2] == ((frame - event) / (fade - 1) if event is not None else 0),
                'endpoint order/count/weight')
    return values


def boundary_rows(path, factor, start, fade, total):
    values = doubles(path.read_bytes()); at = 0; rows = []
    for frame in range(start - 64, total):
        count = factor * (2 if start <= frame < start + fade else 1)
        require(at + 2 + 4 * count <= len(values) and values[at] == frame and values[at + 1] == count,
                'boundary frame/count/extent')
        validate_groups(factor, list(range(count)), count // factor)
        rows.append(values[at + 2:at + 2 + 4 * count]); at += 2 + 4 * count
    require(at == len(values), 'trailing boundary trace')
    return rows


def analyze(directory):
    h = helpers(); b = json.loads((directory / 'binding.json').read_text())
    require(b['action'] == ACTION and b['state'] == 'COMPLETE' and b['exit'] == 0, 'trace completion/action')
    for name, expected in b['files'].items():
        path = pathlib.PurePosixPath(name)
        require(not path.is_absolute() and '..' not in path.parts and ':' not in name and '\\' not in name,
                'trace binding path escape')
        require(identity(directory / name) == expected, 'trace binding identity')
    inventory = {p.relative_to(directory).as_posix() for sub in ('proof', 'native')
                 for p in (directory / sub).rglob('*') if p.is_file()}
    require(inventory == set(b['files']), 'complete trace inventory')
    frozen_name = 'probes/saturation/contracts/baseline-8b7-20261022/SaturationCandidate.cpp'
    if frozen_name in b['sources']:
        import saturation_numerical_stability as stability
        stability.check_frozen(directory / 'proof/source' / pathlib.PurePosixPath(frozen_name).parent)
        for target in ('RackSynthSaturationHistoryTraceCandidate',):
            stability.check_frozen_project(directory / 'proof' / (target + '.vcxproj'), target)
        original = (directory / 'proof/source' / frozen_name).read_bytes()
    else:
        original = (directory / 'proof/source/probes/saturation/SaturationCandidate.cpp').read_bytes()
    require(hashlib.sha256(original).hexdigest() == PINNED, 'original source identity')
    generated = directory / 'proof/SaturationCandidateObserved.cpp'
    if generated.exists():
        require(recover(generated.read_bytes()) == original and
                (directory / 'proof/SaturationCandidateObserved.original.cpp').read_bytes() == original and
                (directory / 'proof/SaturationCandidateObserved.recovered.cpp').read_bytes() == original,
                'instrumented source recovery')
    inp = pathlib.Path(b['input']); f, rate, case = b['factor'], b['rate'], b['case']
    require(h.retained(inp, f, rate, case) == b['input_files'], 'original binding')
    require(b['command'] == [b['coordinator'], '--wait-seconds', '1800', '--run-seconds', '600', '--',
                            str(directory / 'proof/trace.exe'), '--factor', str(f), '--rate', str(rate),
                            '--case', case, '--input', str(inp), '--output', str(directory / 'native')],
            'trace command binding')
    start, fade = 4096 + 2 * rate, math.ceil(.020 * rate); total = start + fade + 128
    manifest = json.loads((directory / 'native/capture.json').read_text())
    require(manifest == dict(schema=1, role='return-trace', completed=True, action=ACTION, factor=f, rate=rate,
                            case=case, prefix=4096, common=2 * rate, fade=fade, settled=128, input=inp.as_posix()),
            'trace manifest')
    qualified, endpoints, boundaries = {}, {}, {}
    for role in h.histories(case):
        stem = directory / 'native' / f'{case}.{role}.output.f64'
        qualified[role] = qualify((inp / stem.name).read_bytes(), stem.read_bytes())
        endpoints[role] = endpoint_rows(pathlib.Path(str(stem) + '.endpoints.f64'), total, f, start, fade,
                                       role != 'fresh' if case.startswith('mode') else role == 'old1')
        boundaries[role] = boundary_rows(pathlib.Path(str(stem) + '.boundary.f64'), f, start, fade, total)
    reconstruction = {role: max(abs(v[11 * frame + 7 + ch] - v[11 * frame + 9 + ch])
                               for frame in range(total) for ch in range(2)) for role, v in endpoints.items()}
    stage = {}
    for other in h.histories(case)[1:]:
        pair = {}
        for ep, label in ((0, 'continued_source'), (1, 'returned_destination')):
            samples = []
            for rel in range(fade):
                a, z = boundaries['old0'][64 + rel], boundaries[other][64 + rel]
                for n in range(f):
                    for ch in range(2):
                        k = 4 * (ep * f + n) + ch
                        samples.append((abs(a[k] - z[k]), rel, n, ch, a[k].hex(), z[k].hex()))
            maximum = max(samples)
            after = max(x[0] for x in samples if x[1] >= 172) if fade > 172 else None
            pair[label] = dict(maximum=maximum, after172_max=after, scalar_samples=len(samples),
                               first_nonzero=next((x for x in samples if x[0] != 0), None))
        pair['native_output'] = difference((inp / f'{case}.old0.output.f64').read_bytes(),
                                           (inp / f'{case}.{other}.output.f64').read_bytes(), start, total)
        a, bvalues = endpoints['old0'], endpoints[other]
        destination_after = []; residual = []; bounds = []
        for rel in range(fade):
            frame = start + rel; w = rel / (fade - 1)
            for ch in range(2):
                av, bv = a[11 * frame:11 * frame + 11], bvalues[11 * frame:11 * frame + 11]
                source = (1 - w) * (av[3 + ch] - bv[3 + ch])
                r = (av[9 + ch] - bv[9 + ch]) - source
                if rel >= 172:
                    destination_after.append(abs(av[5 + ch] - bv[5 + ch])); residual.append(abs(r))
                    # Conservative operation-by-operation binary64 rounding allowance, not a quality tolerance.
                    bound = 0.
                    for row in (av, bv):
                        delta = row[5 + ch] - row[3 + ch]; product = w * delta
                        bound += math.ulp(delta) * abs(w) + math.ulp(product) + math.ulp(row[9 + ch])
                    bound += math.ulp(av[9 + ch] - bv[9 + ch]) + math.ulp(1 - w) * abs(
                        av[3 + ch] - bv[3 + ch]) + math.ulp(source) + math.ulp(r)
                    bounds.append(bound)
        pair['return_decomposition_after172'] = dict(destination_native_max=max(destination_after),
            residual_after_subtracting_weighted_source=max(residual), operation_rounding_bound_max=max(bounds),
            every_residual_within_own_bound=all(x <= y for x, y in zip(residual, bounds)))
        if (directory / 'native' / f'{case}.{other}.output.f64.stages.json').exists():
            pair['depth'] = depth_analysis(directory / 'native' / f'{case}.{other}.output.f64.stages.json', other, f, fade)
        stage[other] = pair
    depth_controls = {}
    for role in h.histories(case):
        path = directory / 'native' / f'{case}.{role}.output.f64.stages.json'
        if path.exists():
            data = depth_analysis(path, role, f, fade)
            depth_controls[role] = dict(raw=data['raw'], counts=data['counts'])
    return dict(factor=f, rate=rate, case=case, qualified=qualified, full_linear_reconstruction_max=reconstruction,
                depth_controls=depth_controls, stages=stage, binding=identity(directory / 'binding.json'))


def run(args):
    h = helpers()
    require(args.input.is_absolute() and args.output.is_absolute() and args.native.is_absolute() and
            args.coordinator.is_absolute(), 'absolute trace paths required')
    inp, out = args.input.resolve(), args.output.resolve()
    require(not out.is_relative_to(inp.parent), 'output must be outside retained cohort')
    require(not out.exists(), 'fresh trace output required')
    inputs = h.retained(inp, args.factor, args.rate, args.case)  # Before any metadata/copy/mkdir.
    build = next((p for p in args.native.parents if (p / 'CMakeCache.txt').is_file()), None)
    require(build is not None, 'configured image required')
    out.mkdir(); proof = out / 'proof'; proof.mkdir()
    import saturation_numerical_stability as stability
    stability.check_frozen()
    sources = [s for s in h.SOURCES if pathlib.Path(s).name not in stability.NUMERICAL]
    sources += ['probes/saturation/contracts/baseline-8b7-20261022/' + n for n in (*stability.NUMERICAL, 'manifest.json')]
    sources += ['tools/saturation_numerical_stability.py', 'probes/saturation/contracts/SaturationHistoryTrace.h',
                           'probes/saturation/contracts/SaturationHistoryTrace.cpp',
                           'probes/saturation/contracts/SaturationReturnOracle.h',
                           'tools/saturation_history_trace.py', 'tests/test_saturation_history_trace.py']
    for name in sources:
        dest = proof / 'source' / name; dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, dest)
    shutil.copy2(args.native, proof / 'trace.exe')
    for name in (TARGET + '.vcxproj', TARGET + 'Candidate.vcxproj', 'CMakeCache.txt'):
        shutil.copy2(build / name, proof / name)
    shutil.copy2(build / ('Release/' + TARGET + 'Candidate.lib'), proof / 'private-candidate.lib')
    for generated in (build / 'saturation-history-trace').glob('SaturationCandidateObserved.*'):
        shutil.copy2(generated, proof / generated.name)
    b = dict(action=ACTION, state='PREPARED', factor=args.factor, rate=args.rate, case=args.case,
             input=str(inp), input_files=inputs, coordinator=str(args.coordinator), sources=sources)
    b['command'] = [str(args.coordinator), '--wait-seconds', '1800', '--run-seconds', '600', '--', str(proof / 'trace.exe'),
                    '--factor', str(args.factor), '--rate', str(args.rate), '--case', args.case,
                    '--input', str(inp), '--output', str(out / 'native')]
    receipt = out / 'binding.json'; receipt.write_text(json.dumps(b, indent=2) + '\n', encoding='utf-8')
    with (out / 'native.log').open('w', encoding='utf-8') as log:
        result = subprocess.run(b['command'], stdout=log, stderr=subprocess.STDOUT, text=True)
        log.write(f'\nexit={result.returncode}\n')
    b.update(exit=result.returncode, state='COMPLETE' if result.returncode == 0 else 'NATIVE_FAILURE')
    b['files'] = {p.relative_to(out).as_posix(): identity(p) for sub in ('proof', 'native')
                  for p in (out / sub).rglob('*') if p.is_file()}
    receipt.write_text(json.dumps(b, indent=2) + '\n', encoding='utf-8')
    report = analyze(out)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument('--analyze', type=pathlib.Path)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--generate', type=pathlib.Path)
    parser.add_argument('--source', type=pathlib.Path)
    for name in ('native', 'coordinator', 'input', 'output'):
        parser.add_argument('--' + name, type=pathlib.Path)
    parser.add_argument('--factor', type=int, choices=(4, 64))
    parser.add_argument('--rate', type=int, choices=(44100, 48000, 96000))
    parser.add_argument('--case')
    args = parser.parse_args()
    if args.generate:
        require(not args.run and not args.analyze, 'one generation action')
        import saturation_numerical_stability as stability
        source = args.source or stability.FROZEN / 'SaturationCandidate.cpp'
        require(source.name == 'SaturationCandidate.cpp', 'historical source filename')
        stability.check_frozen(source.parent)
        original = source.read_bytes()
        generated = generate(original)
        args.generate.parent.mkdir(parents=True, exist_ok=True)
        args.generate.write_bytes(generated)
        args.generate.with_suffix('.original.cpp').write_bytes(original)
        args.generate.with_suffix('.recovered.cpp').write_bytes(recover(generated))
        args.generate.with_suffix('.recovery.json').write_text(json.dumps(dict(original_sha256=PINNED,
            generated_sha256=hashlib.sha256(generated).hexdigest(), recovered_sha256=hashlib.sha256(recover(generated)).hexdigest(),
            byte_exact=True, insertions=generated.count(BEGIN)), indent=2) + '\n', encoding='utf-8')
        return
    require(args.source is None and args.run != bool(args.analyze), 'one trace action required')
    if args.analyze:
        require(args.analyze.is_absolute(), 'absolute retained trace required')
        result = analyze(args.analyze.resolve())
    else:
        require(all(getattr(args, key) is not None for key in
                    ('native', 'coordinator', 'input', 'output', 'factor', 'rate', 'case')), 'explicit trace arguments')
        result = run(args)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
