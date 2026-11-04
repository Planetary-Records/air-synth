"""Source-bound first-return diagnostic; exit zero is never fixture acceptance."""
import argparse
import ast
import csv
import datetime
from decimal import Decimal, localcontext
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys

from saturation_fixture_quotients import (AUDIO, CASE, CORPUS, FIELDS, MANIFEST_PIN,
    qualify_audio, validate_rows, hex_value)
from saturation_numerical_stability import identity, integrated_quotient, require, tlog_records

ROOT = Path(__file__).resolve().parents[1]
CAPTURE = ROOT / 'build/saturation-convergence-followup-20261022/task4-green-driver-04/capture'
ACCEPTANCE = ROOT / 'docs/verification/saturation-product-state-integration-20261022/task3-acceptance.json'
ACCEPTANCE_PIN = '9536f4690ab6a89a080a94cabcffdfcc9410843bd52c72cd67fba077de59c0de'
RESULT_PIN = 'bdc9b0fed2b441a615df5a4d5935e59a52bf55b2bbb9d5ef6f01b24917f1c02c'
MANIFEST = ROOT / 'build/saturation-product-state-integration-20261022/task3-continuation/quality/4/nonalias/frozen-manifest.json'
SOURCE = 'probes/saturation/SaturationCandidate.cpp'
BOUND = Fraction('4e-15')


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def check_identity(path, expected):
    require(identity(path) == expected, 'bound identity changed: ' + str(path))


def double_hash(values):
    return hashlib.sha256(struct.pack('<' + str(len(values)) + 'd', *values)).hexdigest()


def quotient_enclosure(u):
    """Exact rational cosh tail and alternating log bounds; deliberately local domain."""
    require(math.isfinite(u) and abs(u) <= .001, 'finite zero-near quotient domain required')
    if u == 0:
        return Fraction(0), Fraction(0)
    z = abs(Fraction.from_float(u))
    term = Fraction(1)
    total = term
    for k in range(7):
        term = term * z * z / ((2*k+1)*(2*k+2))
        total += term
    next_term = term * z * z / (15*16)
    ratio = z*z/(17*18)
    require(0 <= ratio < 1, 'convergent positive cosh tail required')
    vlo, vhi = total-1, total-1 + next_term/(1-ratio)
    require(0 <= vlo <= vhi < 1, 'alternating log domain required')
    def alternating(v, terms):
        return sum(((-1)**(k+1) * v**k / k for k in range(1, terms+1)), Fraction(0))
    lo, hi = alternating(vlo, 8)/z, alternating(vhi, 7)/z
    return (lo, hi) if u > 0 else (-hi, -lo)


def outward_float(value, upper):
    rounded = float(value)
    require(math.isfinite(rounded), 'finite binary64 endpoint required')
    represented = Fraction.from_float(rounded)
    if (upper and represented < value) or (not upper and represented > value):
        rounded = math.nextafter(rounded, math.inf if upper else -math.inf)
    require(math.isfinite(rounded), 'finite outward endpoint required')
    require(Fraction.from_float(rounded) >= value if upper else Fraction.from_float(rounded) <= value,
            'endpoint rounded inward')
    return rounded


def admissible_interval(lo, hi, mp180):
    require(lo <= mp180 <= hi, 'MP180 witness outside independent rational enclosure')
    # Both centers are covered; this only enlarges the analytical set, never the gate.
    return min(lo, mp180)-BOUND, max(hi, mp180)+BOUND


def load_model(packet, settings):
    fixed = dict(file=CASE, rate='44100', factor='4', mode='0', input_trim='0', output_trim='0',
                 mix='0.75', compensation='0', pre_on='0', post_on='0', lane0_bypass='0',
                 lane0_character='0', lane0_drive='12', lane0_tilt='0', lane0_mix='0.75', lane0_trim='0')
    require(all(settings.get(k) == v for k,v in fixed.items()), 'fixed first-case full/Soft/no-cut/no-tilt settings required')
    require(packet['schema'] == 1 and packet['factor'] == 4 and packet['endian'] == 'little' and
            packet['hash_algorithm'] == 'SHA256', 'native coefficient manifest schema required')
    coefficients = {}
    for row in packet['coefficients']:
        name, values = row['name'], row['values']
        require(name not in coefficients and values and all(type(x) in (int,float) and math.isfinite(x) for x in values),
                'unique finite coefficient arrays required')
        require(double_hash(values) == row['sha256'], 'native coefficient binary64 hash mismatch')
        coefficients[name] = values
    names = dict(up1='iir1-up-direct', up2='iir2-up-direct', down1='iir1-down-direct', down2='iir2-down-direct')
    require(all(name in coefficients for name in names.values()), 'first-return coefficients missing')
    model = {key: coefficients[name] for key,name in names.items()}
    require(all(len(model[key]) == n for key,n in (('up1',5),('down1',5),('up2',3),('down2',3))),
            'fixed four-times direct section layout required')
    require(all(0 < c < 1 for key in names for c in model[key]), 'positive direct coefficients required')
    # gain(db) is the native scalar pow law; the native u check qualifies its exact result.
    model.update(drive_gain=math.pow(10., float(settings['lane0_drive'])/20.),
                 amount=float(settings['lane0_drive'])/36., lane_mix=float(settings['lane0_mix']),
                 global_mix=float(settings['mix']), wet_gain=1., input_gain=1., output_gain=1.)
    require(all(math.isfinite(model[k]) and model[k] > 0 for k in ('drive_gain','amount','lane_mix','global_mix',
                                                                   'wet_gain','input_gain','output_gain')),
            'positive finite first-return gain/mix multipliers required')
    return model


def cascade_zero(value, coefficients):
    for coefficient in coefficients:
        value = coefficient * value + 0.0
    return value


def replay_first(x, q, model):
    d = .5 * x
    t = d
    difference = (1./16.) * q - t
    lane = d + model['lane_mix'] * ((t + model['amount'] * difference) * model['wet_gain'] - d)
    combined = d + model['global_mix'] * (lane-d)
    y = cascade_zero(combined, model['down2'])
    y = .5 * (0.0+y)
    y = cascade_zero(y, model['down1'])
    y = .5 * (0.0+y)
    return y * model['output_gain']


def read_trace(path):
    with path.open(newline='', encoding='utf-8') as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == FIELDS, 'exact ordered native trace schema required')
        rows = list(reader)
    validate_rows(rows)
    return rows


def qualify_first(source, rows, actual, model):
    require(len(source) == 2 and len(actual) == 32768 and len(rows) >= 2, 'complete native stereo audio required')
    native = struct.unpack('<4096d', actual)
    require(all(math.isfinite(v) for v in native), 'finite native recording required')
    results = []
    for channel in range(2):
        row = rows[channel]
        expected = dict(frame='0', order=str(channel), record=str(channel), site='0', channel=str(channel),
                        character='0', kind='1', previous_kind='0', branch='exact_primitive')
        require(all(row[k] == v for k,v in expected.items()), 'first trace identity/site/channel/kind mismatch')
        require(all(hex_value(row[k]) == 0. for k in ('p','prior_primitive','cached_primitive')),
                'first trace zero-state primitive/cache/operand required')
        x = cascade_zero(cascade_zero(source[channel] * model['input_gain'], model['up1']), model['up2'])
        u = model['drive_gain'] * x / (1./16.)
        require(u.hex() == hex_value(row['u']).hex(), 'native first operand replay mismatch')
        current = replay_first(x, hex_value(row['q']), model)
        require(current.hex() == native[channel].hex(), 'native first-output replay mismatch')
        old = replay_first(x, hex_value(row['old_q']), model)
        require(all(v == 0 or math.isfinite(v) and abs(v) >= sys.float_info.min
                    for v in (source[channel], x, u, current, old)), 'normal finite first-return operands required')
        results.append(dict(channel=channel, input=source[channel].hex(), x=x.hex(), u=u.hex(),
                            current_q=row['q'], old_q=row['old_q'], current_replay=current.hex(), old_replay=old.hex(),
                            native=native[channel].hex(), operands=dict(row), native_replay_bit_exact=True))
    require(rows[0]['lane'] == rows[1]['lane'], 'same native lane required')
    return results


def exact(value):
    return dict(numerator=str(value.numerator), denominator=str(value.denominator))


def scalar(value):
    return dict(hex=value.hex(), bits=f'{struct.unpack("<Q", struct.pack("<d",value))[0]:016x}', rational=exact(Fraction.from_float(value)))


def membership(x, model, interval, expected):
    lo, hi = interval
    require(lo <= hi and math.isfinite(expected), 'ordered interval and finite expected sample required')
    qlo, qhi = outward_float(lo, False), outward_float(hi, True)
    lower, upper = replay_first(x, qlo, model), replay_first(x, qhi, model)
    require(all(math.isfinite(v) and abs(v) >= sys.float_info.min for v in (qlo,qhi,lower,upper)),
            'normal finite endpoint chain required')
    require(lower <= upper, 'monotone rounded endpoint replay required')
    inside = lower <= expected <= upper
    return dict(q_rational_lower=exact(lo), q_rational_upper=exact(hi), q_lower=scalar(qlo), q_upper=scalar(qhi),
                lower=scalar(lower), upper=scalar(upper), expected=scalar(expected),
                lower_float=lower, upper_float=upper, expected_inside=inside, excluded=not inside,
                expected_minus_lower=exact(Fraction.from_float(expected)-Fraction.from_float(lower)),
                expected_minus_upper=exact(Fraction.from_float(expected)-Fraction.from_float(upper)),
                decision='included_no_implementation_proved' if inside else 'excluded_under_fixed_first_return')


class Bindings:
    """Copy actual bytes before interpretation and recheck every paired path afterward."""
    def __init__(self, output):
        self.output, self.files = output, {}

    def add(self, path, expected=None):
        path = Path(path).resolve()
        bound = identity(path)
        if expected is not None:
            require(bound == expected, 'bound identity changed: ' + str(path))
        key = str(path)
        if key not in self.files:
            retained = self.output / 'before' / f'{len(self.files):04d}' / path.name
            retained.parent.mkdir(parents=True)
            shutil.copy2(path, retained)
            check_identity(retained, bound)
            self.files[key] = dict(identity=bound, retained=str(retained.relative_to(self.output)))
        else:
            require(self.files[key]['identity'] == bound, 'bound input changed while collecting')
        return path

    def reference(self, record):
        path = Path(record['path'])
        return self.add(path if path.is_absolute() else ROOT/path, record['identity'])

    def packet(self, path, expected=None):
        return json.loads(self.add(path, expected).read_text(encoding='utf-8'))

    def verify(self):
        for path, record in self.files.items():
            check_identity(Path(path), record['identity'])
            check_identity(self.output / record['retained'], record['identity'])

    def save(self):
        write_json(self.output/'input-proof.json', dict(created_utc=utc(), files=self.files,
                   scope='Actual retained bytes; historical producing sources keep their original scope.'))


def function_body(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    level = 1
    end = brace+1
    while level:
        require(end < len(text), 'complete source body required')
        level += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def source_mapping(current, historical):
    # These exact bodies contain the causal first-return and first-admission arithmetic.
    markers = ('static forcedinline __m128d cascadeStereo(', 'static forcedinline void upStereo(',
               'static forcedinline Candidate::Frame downStereo(', 'void up4(', 'Candidate::Frame down4(',
               'void process4Character(', 'void updateCoefficients()', 'void advanceControls()', 'void clear() noexcept')
    mappings = []
    for marker in markers:
        a, b = function_body(current, marker), function_body(historical, marker)
        require(a == b, 'unqualified first-return source mapping: ' + marker)
        mappings.append(dict(marker=marker, bytes=len(a.encode()), sha256=hashlib.sha256(a.encode()).hexdigest(), equality='exact'))
    # ProductState changes only the 64x path and charged inactive cuts in this body.
    a, b = function_body(current, 'Frame processPrepared('), function_body(historical, 'Frame processPrepared(')
    a = a.replace('        if constexpr (Capacity == 64 && !ProductState)\n            if (factor == 64) return processPrepared<64, true>(input);\n', '')
    a = a.replace('<ProductState>', '').replace(', ProductState>', '>')
    require(a == b, 'unqualified processPrepared 4x source mapping')
    mappings.append(dict(marker='Frame processPrepared(', equality='exact_after_explicit_non4x_template_and_wrapper_removal',
                         normalized_sha256=hashlib.sha256(a.encode()).hexdigest()))
    for marker in ('bool Candidate::prepare(', 'bool Candidate::setSettings(', 'Candidate::Frame Candidate::process(',
                   'void Candidate::reset()'):
        def bodies(text):
            positions = [i for i in range(len(text)) if text.startswith(marker,i)]
            return [function_body(text[i:],marker) for i in positions]
        a, b = bodies(current), bodies(historical)
        require(a == b and len(a) == 2, 'both native admission/prepare/reset source variants required: '+marker)
        mappings.append(dict(marker=marker,equality='both_exact',bodies=len(a),sha256=hashlib.sha256('\n'.join(a).encode()).hexdigest()))
    return mappings


def bind_evidence(corpus, capture, manifest, binding):
    acceptance = binding.packet(ACCEPTANCE)
    require(identity(ACCEPTANCE)['sha256'] == ACCEPTANCE_PIN, 'pinned current acceptance required')
    correction = binding.packet(binding.reference(acceptance['current_producer_proof_correction']))
    for record in correction['artifacts'].values():
        binding.add(record['retained'], record['identity'])
    result = binding.packet(capture/'result.json')
    require(identity(capture/'result.json')['sha256'] == RESULT_PIN, 'pinned historical complete capture result required')
    for name, bound in result['raw'].items(): binding.add(capture/name, bound)
    proof = binding.packet(capture/'source-proof.json', result['proof'])
    require(proof['sources'][SOURCE]['sha256'] == '134416edd37ce9a4dbb4e20063b2d7fa0e98dade5f297ee0685a8ffce83f6e30',
            'historical numerical producer source pin required')
    for name,bound in proof['files'].items(): binding.add(capture/'proof'/name, bound)
    require(result['native_receipt']['exit'] == 1, 'historical capture failed fixture exit required')
    for name,target in proof['projects'].items():
        flags = target['flags']
        require(flags['Optimization'] == 'MaxSpeed' and flags['LanguageStandard'] == 'stdcpp20' and
                flags['RuntimeLibrary'] == 'MultiThreadedDLL', 'historical native compiler model required')
        logs = [capture/'proof'/p for p in proof['files'] if p.startswith('build/'+name+'.dir/') and p.endswith('/CL.command.1.tlog')]
        require(len(logs) == 1, 'actual historical compiler commands required')
        commands = tlog_records(logs[0])
        for lines in commands.values():
            command = ' '.join(lines).upper()+' '
            require(all(flag in command for flag in ('/FP:PRECISE ', '/O2 ', '/MD ', '/STD:C++20 ')) and
                    not any(flag in command for flag in ('/FP:FAST','/FP:CONTRACT','/ARCH:')), 'actual historical FP/ISA contract required')
    native_packet = binding.packet(capture/'native/quotients.json')
    require(native_packet['schema'] == 'saturation-fixture-quotients-v1' and native_packet['records'] == 16384 and
            native_packet['case'] == CASE and native_packet['partition'] == 1 and native_packet['frames'] == 2048 and native_packet['channels'] == 2,
            'complete original native packet required')
    original = binding.packet(corpus/'manifest.json')
    require(identity(corpus/'manifest.json')['sha256'] == MANIFEST_PIN and len(list(corpus.iterdir())) == 27 and
            set(p.name for p in corpus.iterdir()) == set(original['files']) | {'manifest.json'}, 'pinned complete 27-file corpus required')
    for name,bound in original['files'].items(): binding.add(corpus/name, bound)
    require(original['initial_sequence'] == 'fresh Candidate; setSettings; prepare(rate,4); reset; process saved input; no edits or warmup'
            and original['reset'] == 'zero audio histories and settled admitted targets', 'original zero-state admission required')
    require(len(acceptance['immutable_inputs']['fixtures']) == 31, 'broader 31-file immutable fixture inventory required')
    for name,bound in acceptance['immutable_inputs']['fixtures'].items(): binding.add(ROOT/name, bound)
    for field,path in [('manifest',corpus/'manifest.json'),('input',corpus/'input.f64'),('settings',corpus/'settled-settings.csv'),
                       ('actual',capture/'native'/AUDIO),('records',capture/'native/q-records.csv')]:
        require(native_packet[field+'_sha256'] == identity(path)['sha256'], 'native raw/settings identity mismatch: '+field)
    for name in ('fixture-tests-01', 'fixture-diagnosis-01'):
        command = acceptance['commands'][name]
        receipt_path = binding.reference(command['receipt'])
        receipt = binding.packet(receipt_path)
        require(command['exit_code'] == receipt['exit_code'] == 1 and command['argv'] == receipt['argv'], 'actual current fixture failure receipt required')
        for snapshot in (receipt['source_snapshot'],receipt['helper_snapshot']):
            for record in snapshot.values(): binding.add(record['copy'], {k:record[k] for k in ('bytes','sha256')})
        # The receipt stores hashes and the paired raw stdout/stderr beside itself.
        for stream in ('stdout','stderr'):
            binding.add(receipt_path.with_name(name+'.'+stream+'.log'), command[stream])
    directory = Path(acceptance['commands']['fixture-diagnosis-01']['argv'][-1])
    binding.add(directory/'comparisons.csv', acceptance['fixtures']['comparisons_csv'])
    binding.add(directory/'diagnosis.json', acceptance['fixtures']['diagnosis'])
    with (directory/'comparisons.csv').open(newline='') as stream: comparisons = list(csv.DictReader(stream))
    require(len(comparisons) == 100, 'complete original comparison rows required')
    current_proof_path = binding.reference(acceptance['raw']['continuation/frozen-01/producer-binding.json'])
    current_proof = binding.packet(current_proof_path)
    archive = current_proof_path.parent
    archive_manifest = binding.packet(archive/'manifest.json',current_proof['archive'])
    for name,bound in current_proof['sources'].items():
        relative = Path(name).relative_to(ROOT)
        if relative.as_posix() not in (SOURCE,'probes/saturation/SaturationCandidate.h','probes/saturation/SaturationGain.h',
                                      'probes/saturation/SaturationMath.h','probes/saturation/contracts/SaturationFixtureTests.cpp'):
            continue
        if relative.as_posix() == 'probes/saturation/contracts/SaturationFixtureTests.cpp':
            # This unchanged harness is retained in the pinned earlier capture, not this source-only archive.
            binding.add(capture/'proof'/relative,bound)
            binding.add(ROOT/relative,bound)
        else:
            binding.add(archive/relative,bound)
            require(archive_manifest['files'][relative.as_posix()] == bound, 'current producer archived source identity required')
    for target in ('RackSynthSaturationCandidate','RackSynthSaturationFixtureTests','RackSynthSaturationFixtureDiagnosis'):
        for name,record in current_proof['actual_records'][target].items():
            path = binding.add(archive/record['path'],record['identity'])
            require(tlog_records(path) == record['records'], 'actual current compiler/link record equality required')
            if name == 'CL.command.1.tlog':
                for lines in record['records'].values():
                    command = ' '.join(lines).upper()+' '
                    require(all(flag in command for flag in ('/FP:PRECISE ','/O2 ','/MD ','/STD:C++20 ')) and
                            not any(flag in command for flag in ('/FP:FAST','/FP:CONTRACT','/ARCH:')), 'actual current FP/ISA contract required')
        if target != 'RackSynthSaturationCandidate':
            image = current_proof['images'][target]
            binding.add(archive/Path(image['path']).relative_to(ROOT),image['identity'])
            require(current_proof['expected_links'][target] == 'RackSynthSaturationCandidate', 'actual current normal-Candidate linkage required')
            linked = {line.upper() for lines in current_proof['actual_records'][target]['link.read.1.tlog']['records'].values() for line in lines}
            candidates = {line for line in linked if 'SATURATION' in line and line.endswith('CANDIDATE.LIB')}
            require(candidates == {str(ROOT/'build/windows-debug/Release/RackSynthSaturationCandidate.lib').upper()},
                    'actual current selected normal library required')
    prior = Path(acceptance['fixtures']['prior'])
    first_audio = None
    for row, comparison in zip(acceptance['fixtures']['rows'], comparisons):
        filename = row['case']+'.partition-'+str(row['partition'])+'.actual.f64'
        expected = 'event-expected.f64' if row['case'] == 'event-characterization' else 'serial-expected.f64' if row['case'] == 'serial-characterization' else row['case']
        actual = binding.add(directory/filename, row['actual'])
        old = binding.add(prior/filename, row['actual'])
        binding.add(corpus/expected, row['expected'])
        require(actual.read_bytes() == old.read_bytes(), 'complete current/prior fixture audio equality required')
        actual_samples,expected_samples = (struct.unpack('<4096d',p.read_bytes()) for p in (actual,corpus/expected))
        actual_bits,expected_bits = (struct.unpack('<4096Q',p.read_bytes()) for p in (actual,corpus/expected))
        require(all(math.isfinite(v) for v in (*actual_samples,*expected_samples)), 'finite original fixture recordings required')
        maximum = max(abs(a-b) for a,b in zip(actual_samples,expected_samples))
        different = sum(a != b for a,b in zip(actual_bits,expected_bits))
        passed = maximum <= 1e-12 and different == 0
        require((maximum,different,passed) == (row['max_absolute'],row['bit_differences'],row['passed']) and
                comparison['case'] == row['case'] and int(comparison['partition']) == row['partition'] and
                comparison['expected_file'] == expected and comparison['actual_file'] == filename and
                float(comparison['max_absolute']) == maximum and int(comparison['bit_differences']) == different and
                comparison['pass'] == str(passed).lower(), 'independent original fixture metrics/gate mismatch')
        if row['case'] == CASE and row['partition'] == 1: first_audio = actual
    require(first_audio is not None and len(acceptance['fixtures']['rows']) == 100, 'all 100 original comparisons required')
    qualify_audio((capture/'native'/AUDIO).read_bytes(), first_audio.read_bytes())
    quality = next(q for q in acceptance['quality']['complete_untransitioned'] if q['factor'] == 4)
    qproof = binding.packet(binding.reference(quality['verified_proof']))
    retained = Path(qproof['retained'])
    for name,bound in qproof['files'].items(): binding.add(retained/name, bound['identity'])
    cohort = next(q for q in quality['cohorts'] if q['cohort'] == 'nonalias')
    packet = binding.packet(binding.reference(cohort['packet']))
    index_path = binding.reference(cohort['index'])
    require(qproof['verified_indexes'][str(index_path.parent)] == cohort['index']['identity'], 'qualified historical manifest index required')
    with index_path.open(newline='') as stream:
        indexes = list(csv.DictReader(stream))
    entry = next(row for row in indexes if row['file'] == 'frozen-manifest.json')
    binding.add(manifest, dict(bytes=int(entry['bytes']),sha256=entry['sha256']))
    coefficients = binding.packet(manifest)
    require(packet['frozen_manifest_sha256'] == identity(manifest)['sha256'], 'native historical coefficient manifest binding required')
    for source in coefficients['sources']:
        key = 'probes/saturation/'+source['file']
        require(qproof['sources'][key]['sha256'] == source['sha256'], 'manifest producing-source relationship required')
    # Current numerical sources are identical to the manifest phase. Feasibility/plumbing has its own later scope.
    for key in (SOURCE, 'probes/saturation/SaturationGain.h', 'probes/saturation/SaturationMath.h',
                'probes/saturation/contracts/SaturationFixtureTests.cpp'):
        expected = acceptance['continuation_source_binding']['sources'][str((ROOT/key).resolve())]
        binding.add(ROOT/key, expected)
    historical = (capture/'proof'/SOURCE).read_text()
    current = (ROOT/SOURCE).read_text()
    require(current_proof['sources'][str((ROOT/SOURCE).resolve())] == acceptance['continuation_source_binding']['sources'][str((ROOT/SOURCE).resolve())],
            'actual current fixture producer/current numerical source relationship required')
    mapping = source_mapping(current, historical)
    require(current == (retained/SOURCE).read_text(), 'current/manifest numerical source equality required')
    for name in ('tools/saturation_fixture_quotients.py', 'probes/saturation/SaturationGain.h','probes/saturation/SaturationMath.h'):
        # The numerical oracle and unchanged helper declarations keep their old identity.
        binding.add(ROOT/name, proof['files'][name])
    oracle = 'tools/saturation_numerical_stability.py'
    binding.add(ROOT/oracle, acceptance['continuation_source_binding']['sources'][str((ROOT/oracle).resolve())])
    def functions(path):
        return {node.name: ast.dump(node, include_attributes=False) for node in ast.parse(path.read_text()).body
                if isinstance(node, ast.FunctionDef)}
    now, then = functions(ROOT/oracle), functions(capture/'proof'/oracle)
    for name in ('integrated_quotient','midpoint','original_shape'):
        require(now[name] == then[name], 'unchanged mathematical oracle body required: '+name)
        mapping.append(dict(marker=oracle+'/'+name,equality='exact_AST',sha256=hashlib.sha256(now[name].encode()).hexdigest()))
    return acceptance, result, coefficients, mapping


def analyze(corpus: Path, capture: Path, manifest: Path, output: Path) -> dict:
    require(all(p.is_absolute() for p in (corpus,capture,manifest,output)) and not output.exists(),
            'absolute inputs and a new output directory required')
    require(not any(output.resolve().is_relative_to(p.resolve())
                    for p in (corpus,capture,manifest,CORPUS,CAPTURE,MANIFEST.parent)),
            'output must not overlap an input tree')
    output.mkdir(parents=True)
    write_json(output/'attempt.json', dict(start_utc=utc(), argv=sys.argv, executable=sys.executable, python=sys.version,
               corpus=str(corpus),capture=str(capture),manifest=str(manifest),output=str(output),status='started'))
    binding = Bindings(output)
    try:
        acceptance, old_result, packet, mapping = bind_evidence(corpus,capture,manifest,binding)
        rows = read_trace(capture/'native/q-records.csv')
        with (corpus/'settled-settings.csv').open(newline='') as stream: settings = next(csv.DictReader(stream))
        model = load_model(packet,settings)
        mp = binding.packet(capture/'all-record-mp.json')
        require(len(mp['rows']) == len(rows) and all(record['record'] == n and record['kind'] == int(rows[n]['kind'])
                for n,record in enumerate(mp['rows'])), 'complete ordered MP/native branch inventory required')
        binding.add(Path(__file__))
        binding.save()
        # No expected sample participates in reconstruction or qualification.
        source = struct.unpack('<2d',(corpus/'input.f64').read_bytes()[:16])
        qualified = qualify_first(source,rows,(capture/'native'/AUDIO).read_bytes(),model)
        exact_records = [r for r in mp['rows'] if r['kind'] == 1]
        require([r['record'] for r in exact_records] == [0,1,85], 'all three native exact MP records required')
        for record in exact_records:
            row = rows[record['record']]
            require(record['operands'] == row, 'MP/native operand record mismatch')
            with localcontext() as context:
                context.prec = 220
                low = integrated_quotient(0,hex_value(row['u']),hex_value(row['p']),120)
                high = integrated_quotient(0,hex_value(row['u']),hex_value(row['p']),180)
                require(str(low) == record['mp120'] and str(high) == record['mp180'], 'unchanged recorded MP120/180 oracle required')
                require(abs(low-high) <= Decimal('1e-60'), 'original MP convergence gate required')
                require(abs(Decimal.from_float(hex_value(row['q']))-high) <= Decimal('4e-15'), 'original current q accuracy gate required')
                require(abs(Decimal.from_float(hex_value(row['old_q']))-high) > Decimal('4e-15'), 'original observed old q failure changed')
        expected = struct.unpack('<2d',(corpus/CASE).read_bytes()[:16])
        channels = []
        for channel, record in enumerate(qualified):
            u, x = float.fromhex(record['u']), float.fromhex(record['x'])
            lo, hi = quotient_enclosure(u)
            witness = Fraction(Decimal(exact_records[channel]['mp180']))
            admitted = admissible_interval(lo,hi,witness)
            result = membership(x,model,admitted,expected[channel])
            record.update(exact_q_lower=exact(lo),exact_q_upper=exact(hi),mp180=str(Decimal(exact_records[channel]['mp180'])),
                          rational_contains_mp180=True, old_predicts_expected=(float.fromhex(record['old_replay']).hex() == expected[channel].hex()),
                          reachable=result)
            channels.append(record)
        binding.verify()
        result = dict(schema='saturation-fixture-compatibility-v1',status='diagnostic_only',created_utc=utc(),
                      qualification=qualified, channels=channels, model=model,source_mapping=mapping,
                      fixtures=acceptance['fixtures'], original_native_capture_exit=old_result['native_receipt']['exit'],
                      original_exact_mp=dict(exact_records=3,current_passes=3,old_failures=3,absolute_bound='4e-15',convergence_bound='1e-60'),
                      bound_proof=identity(output/'input-proof.json'),inputs_unchanged=True,
                      conclusion='quotient_only_excluded_under_fixed_first_return' if all(c['reachable']['excluded'] for c in channels)
                                 else 'not_excluded_no_implementation_proved',
                      scope='Only first return of original settled44100/full/Soft/partition1 with fixed positive coefficients, zero history and unchanged operations. No arbitrary architecture/history/serial inference.',
                      selected_factor=None,adoptable=False,production='FORCED_BYPASS',cpu_validation='DEFERRED',
                      timing='NOT_RUN',heap='NOT_RUN',cpu_output_integrity='NOT_RUN',requirements='R4-R7 OPEN')
        write_json(output/'result.json',result)
        write_json(output/'completion.json',dict(end_utc=utc(),exit=0,inputs_unchanged=True,files=len(binding.files)))
        return result
    except (ValueError,OSError,KeyError,TypeError,StopIteration,OverflowError) as error:
        binding.save()
        write_json(output/'refusal.json',dict(end_utc=utc(),status='REFUSED',error_type=type(error).__name__,error=str(error),
                                            compatibility_conclusion=None,fixture_acceptance=None))
        raise ValueError(str(error)) from error


def main():
    parser = argparse.ArgumentParser(description=__doc__,allow_abbrev=False)
    parser.add_argument('--analyze', action='store_true', required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    try:
        result = analyze(CORPUS,CAPTURE,MANIFEST,args.output)
        print('COMPATIBILITY_DIAGNOSTIC',result['conclusion'],'original_fixture_failures',result['fixtures']['failed'])
    except (ValueError,OSError,KeyError,TypeError) as error:
        parser.exit(2,'REFUSED '+str(error)+'\n')


if __name__ == '__main__': main()
