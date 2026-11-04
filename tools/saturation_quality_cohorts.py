"""Validate complete quality cases and retained producer proof; never run native work."""
import argparse
from collections import Counter
import csv
import datetime
import json
import math
from pathlib import Path
import shutil
import sys
import re

from saturation_numerical_stability import (identity, numerical_sources, project_record,
    producer_flags, require, tlog_records)

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('nonalias', 'alias-44100', 'alias-48000', 'alias-96000')
SPLIT_NAMES = NAMES[:3] + ('alias-96000-soft', 'alias-96000-glow', 'alias-96000-dense')
RATES = (44100, 48000, 96000)
STYLES = ('soft', 'glow', 'dense')
LEVELS = (1 / 256, 1 / 64, 1 / 16)
CORNERS = ((180, 3500), (2000, 500), (40, 16000))
NONALIAS = ('neutral-frequency-group-delay.csv', 'neutral-null-layer-controls.csv',
    'compensation.csv', 'character-harmonics.csv', 'dense-recurrence.csv',
    'dense-history-and-link.csv', 'burst-segments.csv', 'operating-grid.csv',
    'quality-observations.csv')
GATES = ('analysis_calibration', 'neutral_response_valid', 'crossover_only_unity_0_1db',
    'combined_neutral_raw_input_envelope', 'neutral_wet_reference_null', 'two_identical_layer_sum',
    'mismatched_raw_dry_control', 'naive_uncompensated_split_control',
    'compensation_all_219_integer_half_steps', 'character_spectrum_valid', 'soft_symmetry',
    'glow_asymmetry', 'glow_dc', 'dense_1e_attack_release', 'dense_sag_range',
    'dense_history_audio', 'dense_stereo_swap', 'operating_grid_finite',
    'alias_reference_qualification', 'alias_candidate_and_improvement',
    'unoversampled_1x_negative_control', 'mismatched_dry_reference_control')
SOURCE_NAMES = ('SaturationCandidate.h', 'SaturationCandidate.cpp', 'SaturationCalibration.h',
    'SaturationAnalysis.h', 'SaturationAnalysis.cpp', 'SaturationAnalysisTests.cpp',
    'SaturationProbeTests.cpp', 'SaturationFeasibility.cpp', 'SaturationWideNetwork.cpp',
    'SaturationWideNetwork.h', 'SaturationMath.h', 'SaturationGain.h',
    'contracts/SaturationNumericalWitness.h', 'SaturationProductState.h','SaturationGainRuntime.h','contracts/SaturationOptimizationWitness.h')
PROOF_SCHEMA = 'saturation-quality-producer-v1'
COHORT_SCHEMA = 'saturation-quality-cohort-v1'


def write_json(path, value):
    path.write_bytes((json.dumps(value, indent=2, allow_nan=False) + '\n').encode('utf-8'))


def load_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate JSON key: ' + key)
            result[key] = value
        return result
    result = json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique,
                        parse_constant=lambda value: (_ for _ in ()).throw(ValueError('nonfinite JSON: ' + value)))
    require(isinstance(result, dict), 'JSON object required')
    return result


def number(row, field, infinite=False):
    try:
        value = float(row[field])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError('invalid numeric field: ' + field) from error
    require(not math.isnan(value) and (infinite or math.isfinite(value)), 'nonfinite metric: ' + field)
    return value


def flag(row, field):
    value = number(row, field)
    require(value in (0, 1), 'invalid boolean field: ' + field)
    return bool(value)


def key(row, fields):
    return tuple(row[field] if field in ('style', 'phase', 'segment', 'recording', 'name')
                 else number(row, field) for field in fields)


def keyed(rows, fields, expected):
    try:
        keys = [key(row, fields) for row in rows]
    except (KeyError, TypeError) as error:
        raise ValueError('missing case key') from error
    require(Counter(keys) == Counter(expected), 'missing/duplicate/unknown case keys: ' + ','.join(fields))
    return dict(zip(keys, rows))


def alias_scope(name):
    require(name in NAMES[1:] + SPLIT_NAMES[3:], 'known alias cohort required')
    return ((int(name.split('-')[1]),),
            (name.rsplit('-', 1)[1],) if name in SPLIT_NAMES[3:] else STYLES)


def alias_keys(rates=RATES, styles=STYLES):
    return [(r, m, s, hz) for r in rates for m in range(2) for s in styles for hz in (1760, 3520, 7040)]


def calibration_gate(rows, include_naive=False):
    expected = [('calibration',), ('naive_split',)] if include_naive else [('calibration',)]
    observations = keyed(rows, ('name',), expected)
    row = observations['calibration',]
    valid = (flag(row, 'summary_valid') and flag(row, 'spectrum_valid') and flag(row, 'result_valid') and
             abs(number(row, 'rms') - .4 / math.sqrt(2)) < 1e-12 and abs(number(row, 'unwanted_dbc') + 20) < 1e-6)
    return gate(GATES[0], valid, not valid)


def gate(name, passed, invalid=False):
    return dict(name=name, **{'pass': bool(passed)}, invalid=bool(invalid), detail='recomputed from complete original case metrics')


def dbc(power, fundamental):
    # Match the original binary64 ratio, including underflow before log10.
    ratio = power / fundamental
    return -math.inf if ratio == 0 else 10 * math.log10(ratio)


def alias_gates(rows, observations, rates=RATES, styles=STYLES):
    fields = ('sample_rate', 'mode', 'style', 'requested_hz')
    original = keyed(rows, fields, alias_keys(rates, styles))
    observed = keyed(observations, fields, alias_keys(rates, styles))
    valid = accepted_all = dry_all = True
    eligible_count = 0
    failures = {(mode, style): False for mode in range(2) for style in styles}
    for case, row in original.items():
        obs = observed[case]
        rate, mode, style, requested = case
        bin_ = number(row, 'bin')
        nearest = int(requested / rate * 65536 + .5)
        if nearest % 2 == 0:
            nearest += 1 if requested / rate * 65536 >= nearest else -1
        require(bin_ == nearest and number(row, 'exact_hz') == rate * bin_ / 65536, 'original coherent bin/frequency')
        ref = number(row, 'reference_unwanted_dbc', True)
        candidate = number(row, 'candidate_unwanted_dbc', True)
        one_x = number(row, 'one_x_unwanted_dbc', True)
        improvement = number(row, 'improvement_db', True)
        require(improvement == one_x - candidate, 'alias improvement metric disagreement')
        for prefix, measured in (('candidate', candidate), ('one_x', one_x)):
            power = number(obs, prefix + '_power', True)
            fundamental = number(obs, prefix + '_fundamental')
            require(power >= 0, 'invalid unwanted power')
            require(fundamental > 0, 'invalid fundamental power')
            require(measured == dbc(power, fundamental), prefix + ' power/dBc disagreement')
        result_valid = ref <= -100
        negative_valid = ref <= -100
        require(flag(row, 'valid') == flag(obs, 'result_valid') == result_valid and
                flag(obs, 'negative_valid') == negative_valid, 'alias validity disagreement')
        accepted = (result_valid and negative_valid and math.isfinite(one_x) and
                    number(obs, 'one_x_power', True) > 0 and candidate <= -50 and candidate <= one_x - 10)
        require(flag(row, 'pass') == accepted, 'forged alias acceptance/pass')
        mismatch = number(row, 'mismatched_dry_dbc', True)
        mismatch_rejected = flag(obs, 'mismatch_valid') and mismatch > -50
        require(flag(row, 'mismatched_control_failed') == mismatch_rejected, 'dry-mask result disagreement')
        try:
            harmonics = [int(value) for value in row['legitimate_bins'].split(';') if value]
        except (ValueError, KeyError) as error:
            raise ValueError('invalid legitimate harmonic mask') from error
        require(len(harmonics) == len(set(harmonics)) and all(0 < h < 32768 and h % bin_ == 0 for h in harmonics),
                'unknown/duplicate legitimate harmonic bins')
        eligible = any(h != bin_ and h * rate / 65536 < min(16000, .4 * rate) for h in harmonics)
        require(flag(obs, 'eligible_dry_mask') == eligible, 'dry-mask eligibility disagreement')
        if eligible:
            eligible_count += 1
            dry_all = dry_all and mismatch_rejected
        failures[mode, style] |= negative_valid and one_x > -50
        valid = valid and result_valid and negative_valid
        accepted_all = accepted_all and accepted
    return [gate(GATES[18], valid, not valid), gate(GATES[19], accepted_all and valid, not valid),
            gate(GATES[20], all(failures.values())), gate(GATES[21], dry_all and eligible_count > 0)]


def nonalias_gates(tables):
    for filename in NONALIAS:
        if filename == 'quality-observations.csv': continue
        for row in tables[filename]:
            for field in row:
                if field not in ('style', 'phase', 'segment', 'recording'):
                    number(row, field, infinite=field.endswith('_dbc'))
    result = []
    def add(name, passed, invalid=False): result.append(gate(name, passed, invalid))
    observations = keyed(tables['quality-observations.csv'], ('name',), [('calibration',), ('naive_split',)])
    result.append(calibration_gate(tables['quality-observations.csv'], include_naive=True))
    neutral = tables['neutral-frequency-group-delay.csv']
    # The original loop retains duplicate endpoints and all 166 rows per rate/corner.
    expected_neutral = []
    for rate in RATES:
        top = min(18000, .4 * rate)
        for low, high in CORNERS:
            frequencies = [30, low, high, min(15000, .35 * rate), top]
            frequencies += [30 * math.pow(top / 30, i / 160) for i in range(161)]
            expected_neutral.extend((rate, low, high, hz) for hz in frequencies)
    keyed(neutral, ('sample_rate', 'low_requested', 'high_requested', 'frequency_hz'), expected_neutral)
    cross = combined = True
    for row in neutral:
        rate, hz = number(row, 'sample_rate'), number(row, 'frequency_hz')
        require(30 <= hz <= min(18000, .4 * rate), 'neutral frequency range')
        full, three = number(row, 'full_raw_input_db'), number(row, 'three_raw_input_db')
        crossover = number(row, 'crossover_only_db')
        require(crossover == three - full, 'crossover metric disagreement')
        for field in ('full_phase_rad', 'three_phase_rad', 'full_group_delay_frames', 'three_group_delay_frames'): number(row, field)
        cross = cross and abs(crossover) <= .1
        bound = .5 if hz <= min(15000, .35 * rate) + 1e-8 else 1
        combined = combined and abs(full) <= bound and abs(three) <= bound
    add(GATES[1], True); add(GATES[2], cross); add(GATES[3], combined)
    nulls = keyed(tables['neutral-null-layer-controls.csv'], ('sample_rate', 'mode', 'low_requested', 'high_requested'),
        [(r, m, low, high) for r in RATES for low, high in CORNERS for m in range(2)])
    add(GATES[4], all(number(r, 'max_wet_dry_null') <= 1e-10 for r in nulls.values()))
    add(GATES[5], all(number(r, 'max_identical_layer_error') <= 2e-10 for r in nulls.values()))
    add(GATES[6], all(number(r, 'max_mismatched_raw_dry_error') > 1e-7 / 16 for r in nulls.values()))
    add(GATES[7], number(observations['naive_split',], 'worst_db') > .1)
    compensation = keyed(tables['compensation.csv'], ('style', 'drive_db'), [(s, n / 2) for s in STYLES for n in range(73)])
    comp_pass = True
    for row in compensation.values():
        require(number(row, 'baseline_rms') > 0 and number(row, 'uncompensated_rms') >= 0 and
                number(row, 'compensated_rms') > 0, 'compensation signal validity')
        error = number(row, 'error_db')
        require(error == 20 * math.log10(number(row, 'compensated_rms') / number(row, 'baseline_rms')), 'compensation error metric disagreement')
        accepted = abs(error) <= .5
        require(flag(row, 'pass') == accepted, 'forged compensation pass')
        table = number(row, 'table_db')
        require(flag(row, 'clamp_contact') == (table <= -18 or table >= 6), 'compensation clamp observation')
        comp_pass = comp_pass and accepted
    add(GATES[8], comp_pass)
    characters = keyed(tables['character-harmonics.csv'], ('sample_rate', 'style', 'peak', 'harmonic_order'),
        [(r, s, level, h) for r in RATES for level in LEVELS for s in STYLES for h in range(1, 109)])
    grouped = {}
    for case, row in characters.items():
        require(number(row, 'fundamental_bin') == 301 and number(row, 'rms') > 0, 'character signal validity')
        number(row, 'harmonic_dbc', True)
        values = (number(row, 'mean'), number(row, 'rms'), number(row, 'even_h2_h4_dbc', True))
        group = case[:3]
        require(group not in grouped or grouped[group] == values, 'character repeated summary disagreement')
        grouped[group] = values
    soft = grouped[48000, 'soft', 1 / 64][2]; glow = grouped[48000, 'glow', 1 / 64]
    add(GATES[9], True); add(GATES[10], soft <= -100)
    add(GATES[11], glow[2] >= -60 and glow[2] - soft >= 20)
    add(GATES[12], abs(glow[0]) <= 1e-5 / 16)
    recurrence = keyed(tables['dense-recurrence.csv'], ('sample_rate', 'factor', 'phase'),
        [(r, f, phase) for r in RATES for f in (1, 2, 4, 8, 64) for phase in ('attack', 'release')])
    rec_pass = sag_pass = True
    for (rate, factor, phase), row in recurrence.items():
        require(number(row, 'samples') == math.ceil((.01 if phase == 'attack' else .4) * rate * factor), 'dense recurrence extent')
        for field in ('retained_envelope', 'closed_form_envelope'): number(row, field)
        # The release row contains the original cumulative attack+release worst error.
        if phase == 'release': rec_pass = rec_pass and number(row, 'worst_relative_error') <= 1e-10
        else: number(row, 'worst_relative_error')
        if phase == 'release': sag_pass = sag_pass and number(row, 'min_sag_db') >= -6 - 1e-12 and number(row, 'max_sag_db') <= 1e-12
        else: number(row, 'min_sag_db'); number(row, 'max_sag_db')
    add(GATES[13], rec_pass); add(GATES[14], sag_pass)
    histories = keyed(tables['dense-history-and-link.csv'], ('sample_rate',), [(r,) for r in RATES])
    add(GATES[15], all(number(r, 'first_20ms_max_difference') > 1e-7 / 16 and number(r, 'after_1s_max_difference') <= 1e-5 / 16 for r in histories.values()))
    add(GATES[16], all(number(r, 'swap_max_error') <= 1e-12 for r in histories.values()))
    operating = keyed(tables['operating-grid.csv'], ('sample_rate', 'mode', 'style', 'drive', 'peak', 'corner_case', 'cuts_enabled'),
        [(r, m, s, d, level, c, cut) for r in RATES for m in range(2) for s in STYLES for d in (0, 6, 18, 36)
         for level in LEVELS for c in range(3) for cut in range(2)])
    operating_valid = True
    for case, row in operating.items():
        rate = case[0]
        for field in ('low_requested', 'high_requested', 'low_effective', 'high_effective', 'pre_cut_hz', 'post_cut_hz', 'rms', 'mean'): number(row, field)
        require(number(row, 'warmup_frames') == int(.25 * rate) and number(row, 'measured_frames') == 4096, 'operating original extent')
        operating_valid = operating_valid and flag(row, 'finite')
    add(GATES[17], operating_valid, not operating_valid)
    labels = ('initial_silence', 'fresh_equal_burst', 'independent_recovery', 'sustained_drive',
              'recovery_after_prior_drive', 'repeated_equal_burst', 'final_recovery')
    bursts = keyed(tables['burst-segments.csv'], ('sample_rate', 'style', 'level', 'segment'),
        [(r, s, level, label) for r in RATES for s in STYLES for level in LEVELS for label in labels])
    for rate in RATES:
        for style in STYLES:
            for level in LEVELS:
                frame = 0
                for label, seconds, peak in zip(labels, (2, .1, 1, 2, 1, .1, 1), (0, level, 0, 1 / 16, 0, level, 0)):
                    row = bursts[rate, style, level, label]; count = round(seconds * rate)
                    require(number(row, 'start_frame') == frame and number(row, 'frames') == count and
                            number(row, 'input_peak') == peak and row['recording'] ==
                            f'burst-{rate}-0-{style}-18-{level:g}.f64', 'burst original segments/recording')
                    frame += count
    return result


def quality_gates(tables):
    result = nonalias_gates(tables) + alias_gates(tables['aliases.csv'], tables['alias-observations.csv'])
    require(tuple(g['name'] for g in result) == GATES, 'original22 gate inventory')
    return result


def recording_spec(name, tables):
    """Original raw membership and full extents; no samples are rendered or substituted."""
    spec = {}
    def add(filename, rate, frames=65536, warm=None):
        require(filename not in spec, 'duplicate expected raw recording')
        spec[filename] = (rate, frames, 2 * rate if warm is None else warm)
    if name != 'nonalias':
        for row in tables['aliases.csv']:
            rate, mode = int(number(row, 'sample_rate')), int(number(row, 'mode'))
            base = f"alias-{rate}-{mode}-{row['style']}-36-0.015625-{int(number(row, 'bin'))}"
            for suffix in ('candidate', 'reference64', 'negative1x', 'mismatched-dry64'):
                add(base + '-' + suffix + '.f64', rate)
        return spec
    for rate in RATES:
        for low, high in CORNERS:
            for mode in ('full', 'three'): add(f'impulse-{mode}-{rate}-{low}-{high}.f64', rate)
            for mode in range(2): add(f'two-layer-{rate}-{mode}-{low}.f64', rate)
        for level in LEVELS:
            for style in STYLES:
                add(f'character-{rate}-0-{style}-18-{level:g}.f64', rate)
                add(f'burst-{rate}-0-{style}-18-{level:g}.f64', rate, sum(round(s * rate) for s in (2, .1, 1, 2, 1, .1, 1)), 0)
        for prefix in ('dense-preconditioned', 'dense-reset', 'dense-history-difference', 'dense-linked-left', 'dense-linked-right'):
            add(f'{prefix}-{rate}.f64', rate, math.ceil(1.1 * rate), 2 * rate if prefix == 'dense-preconditioned' else 0)
    add('negative-naive-uncompensated-LR4.f64', 48000, warm=0)
    for index, row in enumerate(tables['operating-grid.csv']):
        rate = int(number(row, 'sample_rate')); add(f'grid-{index}.f64', rate, 4096, math.ceil(.25 * rate))
    return spec


def input_vector_spec():
    result = []
    def add(name, rate, hz, peak, warm, frames):
        result.append(dict(name=name, sample_rate=rate, frequency=hz, peak=peak,
            warmup_frames=warm, measured_frames=frames))
    for rate in RATES:
        for requested in (1760, 3520, 7040):
            desired = requested * 65536 / rate
            lower = max(1, 2 * math.floor((desired - 1) / 2) + 1)
            bin_ = lower if desired - lower <= lower + 2 - desired else lower + 2
            add('alias-all-modes-styles', rate, rate * bin_ / 65536, 1 / 64, 2 * rate, 65536)
        for level in LEVELS:
            add('character-all-styles', rate, rate * 301 / 65536, level, 2 * rate, 65536)
            add('operating-grid-all-settings', rate, rate * 31 / 4096, level, math.ceil(.25 * rate), 4096)
    add('compensation-all-219-points', 48000, 220, 1 / 64, 48000, 48000)
    return result


def validate_input_vectors(rows):
    fields = ('name', 'sample_rate', 'frequency', 'peak', 'warmup_frames', 'measured_frames')
    keyed(rows, fields, [key(row, fields) for row in input_vector_spec()])
    require(all(re.fullmatch('[0-9a-f]{64}', row['sha256_all_native_input_frames']) for row in rows), 'input vector hash format')


def read_csv(path):
    with path.open(encoding='utf-8', newline='') as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames is not None and len(set(reader.fieldnames)) == len(reader.fieldnames), 'CSV header')
        rows = list(reader)
        require(all(None not in row and None not in row.values() for row in rows), 'incomplete CSV row')
        return rows


def artifacts(directory):
    require(directory.is_absolute() and directory.is_dir(), 'absolute cohort directory required')
    entries = read_csv(directory / 'artifact-hashes.csv')
    names = [r['file'] for r in entries]
    require(names and len(names) == len(set(names)), 'empty/duplicate artifact index')
    require(set(names) == {p.name for p in directory.iterdir() if p.name != 'artifact-hashes.csv'}, 'incomplete/extra artifact index')
    for row in entries:
        name = row['file']
        require(Path(name).name == name and '/' not in name and '\\' not in name and name not in ('.', '..'), 'unsafe artifact path')
        path = directory / name
        require(path.is_file() and not path.is_symlink() and identity(path) == dict(bytes=int(row['bytes']), sha256=row['sha256']), 'tampered artifact: ' + name)
    return {row['file']: row for row in entries}


def source_files(root):
    paths = {'probes/saturation/' + name for name in SOURCE_NAMES} | set(numerical_sources(root)) | {'CMakeLists.txt'}
    require(paths == {'probes/saturation/' + name for name in SOURCE_NAMES} | {'CMakeLists.txt'}, 'runtime source closure must include every numerical dependency')
    return {name: identity(root / name) for name in sorted(paths)}


def absolute(path):
    require(path.is_absolute(), 'explicit absolute path required: ' + str(path))
    return path


def prepare_proof(output, build, consumer, root=ROOT):
    from saturation_gain_parity import gain_import_inputs
    absolute(output); absolute(build); absolute(root)
    require(not output.exists() and consumer in ('RackSynthSaturationQuality', 'RackSynthSaturationTransitionQuality'), 'fresh producer proof/known quality consumer required')
    library = 'RackSynthSaturationTransitionCandidate' if consumer == 'RackSynthSaturationTransitionQuality' else 'RackSynthSaturationCandidate'
    image = build / (consumer + '_artefacts/Release/' + consumer + '.exe')
    sources = source_files(root)
    retained = output.parent / (output.stem + '-files')
    require(not retained.exists(), 'fresh retained proof directory required')
    targets = (consumer, library, 'RackSynthSaturationAnalysis')
    projects = {}
    original_paths = {name: root / name for name in sources}
    original_paths['image.exe'] = image
    for target in targets:
        project = build / (target + '.vcxproj')
        record = project_record(project, 'Release')
        require(record['items'] and all(not item['properties'] for item in record['items']), 'selected project source properties')
        flags = record['flags']
        require(flags['Optimization'] == 'MaxSpeed' and flags['RuntimeLibrary'] == 'MultiThreadedDLL' and
                flags['LanguageStandard'] == 'stdcpp20', 'original selected Release settings')
        forbidden = ('/fp:fast', '/arch:', '/fp:contract', 'avx2')
        require(not any(token in str(flags).lower() for token in forbidden), 'FP/ISA project drift')
        if target == library:
            expected = producer_flags('RackSynthSaturationTransitionCandidate')
            if library == 'RackSynthSaturationCandidate':
                expected['PreprocessorDefinitions'] = expected['PreprocessorDefinitions'].replace(';AIRACK_SATURATION_TRANSITIONS=1', '')
            require({k: v for k, v in flags.items() if k != 'AdditionalIncludeDirectories' and v is not None} ==
                    {k: v for k, v in expected.items() if k != 'AdditionalIncludeDirectories' and v is not None}, 'original numerical project flags')
        projects[target] = record
        original_paths['build/' + project.name] = project
        logs = sorted(p for p in (build / (target + '.dir/Release')).rglob('*.tlog') if p.is_file())
        for log in logs: original_paths['build/' + log.relative_to(build).as_posix()] = log
        if target != consumer:
            original_paths['build/Release/' + target + '.lib'] = build / ('Release/' + target + '.lib')
    original_paths['build/CMakeCache.txt'] = build / 'CMakeCache.txt'
    runtime=gain_import_inputs(build,'Release',root)
    for name,bound in runtime.items():original_paths['gain-runtime/'+bound['sha256']+'-'+Path(name).name]=Path(name)
    require(all(path.is_file() for path in original_paths.values()), 'actual producer files missing')
    require(all((root / name).stat().st_mtime_ns <= image.stat().st_mtime_ns for name in sources), 'producer image predates source closure')
    proof = dict(schema=PROOF_SCHEMA, consumer=consumer, configuration='Release', source_root=str(root), build_root=str(build),
        created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), retained=str(retained), sources=sources,
        image=identity(image), library=library, projects=projects,gain_import=runtime,
        files={name: dict(identity=identity(path), original=str(path)) for name, path in original_paths.items()})
    # Validate real compile and link records before retaining or publishing proof.
    validate_build_proof(proof, original_paths)
    retained.mkdir(parents=True)
    for name, path in original_paths.items():
        destination = retained / name; destination.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, destination)
    write_json(output, proof)
    return proof


def validate_build_proof(proof, paths):
    from saturation_gain_parity import check_retained_gain_import,inspect_pe,inspect_caller
    require(proof['schema'] == PROOF_SCHEMA and proof['configuration'] == 'Release' and
            proof['consumer'] in ('RackSynthSaturationQuality', 'RackSynthSaturationTransitionQuality'), 'producer consumer/configuration')
    consumer = proof['consumer']; library = proof['library']; build = Path(proof['build_root']); root = Path(proof['source_root'])
    runtime=proof['gain_import']
    check_retained_gain_import(runtime,build,'Release',root,{name:paths['gain-runtime/'+bound['sha256']+'-'+Path(name).name] for name,bound in runtime.items()})
    raw=paths['image.exe'].read_bytes();inspect_caller(raw,inspect_pe(raw))
    require(library == ('RackSynthSaturationTransitionCandidate' if consumer == 'RackSynthSaturationTransitionQuality' else 'RackSynthSaturationCandidate'), 'actual linked numerical library variant')
    require(set(proof['projects']) == {consumer, library, 'RackSynthSaturationAnalysis'}, 'exact selected project inventory')
    base_flags = dict(proof['projects'][library]['flags'])
    base_flags['PreprocessorDefinitions'] = base_flags['PreprocessorDefinitions'].replace(';AIRACK_SATURATION_TRANSITIONS=1', '')
    require(base_flags['Optimization'] == 'MaxSpeed' and base_flags['RuntimeLibrary'] == 'MultiThreadedDLL' and
            base_flags['LanguageStandard'] == 'stdcpp20', 'selected Release numerical settings')
    all_dependencies = set()
    for target in (consumer, library, 'RackSynthSaturationAnalysis'):
        record = project_record(paths['build/' + target + '.vcxproj'], 'Release')
        require(record == proof['projects'][target], 'selected project/configuration proof')
        expected_flags = dict(base_flags)
        if target == consumer:
            expected_flags['PreprocessorDefinitions'] = expected_flags['PreprocessorDefinitions'].replace(';NDEBUG;',
                ';NDEBUG;JUCE_STANDALONE_APPLICATION=1;_CONSOLE=1;RACKSYNTH_SATURATION_QUALITY_ONLY=1;')
        elif target == library and library.endswith('TransitionCandidate'):
            expected_flags['PreprocessorDefinitions'] = expected_flags['PreprocessorDefinitions'].replace(';NDEBUG;', ';NDEBUG;AIRACK_SATURATION_TRANSITIONS=1;')
        require({k: v for k, v in record['flags'].items() if k != 'AdditionalIncludeDirectories'} ==
                {k: v for k, v in expected_flags.items() if k != 'AdditionalIncludeDirectories'}, 'original selected consumer/analysis/compiler definitions')
        source_names = ('SaturationFeasibility.cpp',) if target == consumer else ('SaturationAnalysis.cpp',) if target == 'RackSynthSaturationAnalysis' else ('SaturationCandidate.cpp', 'SaturationWideNetwork.cpp')
        require([Path(item['path']) for item in record['items']] == [root / 'probes/saturation' / name for name in source_names], 'exact selected source translation units')
        if target != consumer:
            names = ('SaturationAnalysis.cpp', 'SaturationAnalysis.h') if target == 'RackSynthSaturationAnalysis' else (
                'SaturationCandidate.cpp', 'SaturationCandidate.h', 'SaturationWideNetwork.cpp', 'SaturationWideNetwork.h',
                'SaturationMath.h', 'SaturationGain.h', 'SaturationCalibration.h', 'contracts/SaturationNumericalWitness.h','SaturationGainRuntime.h')
            built = paths['build/Release/' + target + '.lib']
            require(all(paths['probes/saturation/' + name].stat().st_mtime_ns <= built.stat().st_mtime_ns for name in names) and
                    built.stat().st_mtime_ns <= paths['image.exe'].stat().st_mtime_ns, 'compiled library predates source closure or image predates linked library')
        def log(name):
            selected = [path for key_, path in paths.items() if key_.startswith('build/' + target + '.dir/Release/') and Path(key_).name == name]
            require(len(selected) == 1, 'actual compiler/link record missing: ' + target + '/' + name)
            return tlog_records(selected[0])
        commands, reads = log('CL.command.1.tlog'), log('CL.read.1.tlog')
        for item in record['items']:
            source = item['path'].upper()
            require(not item['properties'] and source in commands and source in reads, 'actual selected translation unit')
            command = ' '.join(commands[source]).upper()
            for option in ('/O2 ', '/MD ', '/FP:PRECISE ', '/STD:C++20 ', '/BIGOBJ '):
                require(option in command, 'actual selected compiler option: ' + option)
            require('/ARCH:' not in command and '/FP:FAST' not in command and '/FP:CONTRACT' not in command and 'BASELINE-8B7' not in command and
                    'REFERENCE_TRACE' not in command and 'NUMERICAL_TEST' not in command, 'actual FP/ISA/variant drift')
            if target == library:
                require(('AIRACK_SATURATION_TRANSITIONS=1' in command) == (library.endswith('TransitionCandidate')), 'compiled numerical variant')
            if target == consumer: require('RACKSYNTH_SATURATION_QUALITY_ONLY=1' in command, 'quality-only consumer compile')
            all_dependencies.update(value.upper() for value in reads[source]); all_dependencies.add(source)
        if target == consumer:
            link_commands = '\n'.join(value for values in log('link.command.1.tlog').values() for value in values).upper()
            link_reads = {value.upper() for values in log('link.read.1.tlog').values() for value in values}
            link_writes = {value.upper() for values in log('link.write.1.tlog').values() for value in values}
            for name in (library, 'RackSynthSaturationAnalysis','RackSynthSaturationGainImport'):
                linked = str(build / 'Release' / (name + '.lib')).upper()
                require(linked in link_reads and (name + '.LIB').upper() in link_commands, 'actual image linked library proof: ' + name)
            image_path = str(build / (consumer + '_artefacts/Release/' + consumer + '.exe')).upper()
            require(image_path in link_writes and image_path in link_commands, 'actual link output image proof')
    required = set(proof['sources']) - {'CMakeLists.txt', 'probes/saturation/SaturationAnalysisTests.cpp', 'probes/saturation/SaturationProbeTests.cpp'}
    require(all(str(root / name).upper() in all_dependencies for name in required), 'compiled numerical/harness dependency closure')


def read_proof(path, verified=True):
    absolute(path)
    proof = load_json(path)
    require(proof['schema'] == PROOF_SCHEMA and (not verified or 'verified_utc' in proof), 'verified producer proof required')
    require(all(isinstance(proof[field], dict) for field in ('files', 'sources', 'projects')), 'producer proof mappings required')
    require(all(isinstance(record, dict) and isinstance(record['identity'], dict) and isinstance(record['original'], str)
                for record in proof['files'].values()), 'producer file records required')
    require(all(isinstance(record, dict) for record in proof['sources'].values()), 'producer source identities required')
    for record in proof['projects'].values():
        require(isinstance(record, dict) and isinstance(record['flags'], dict) and isinstance(record['items'], list),
                'producer project containers required')
        require(isinstance(record['flags']['PreprocessorDefinitions'], str) and
                all(value is None or isinstance(value, str) for value in record['flags'].values()), 'producer compiler flag strings required')
        require(all(isinstance(item, dict) and isinstance(item['properties'], list) for item in record['items']),
                'producer project item records required')
    if verified:
        previous = proof['pre_capture_proof']
        require(isinstance(previous, dict) and isinstance(previous['identity'], dict) and
                isinstance(proof['verified_indexes'], dict) and all(isinstance(record, dict) for record in proof['verified_indexes'].values()),
                'pre/post producer proof containers required')
        before = absolute(Path(previous['path']))
        require(identity(before) == previous['identity'] and load_json(before) ==
                {k: v for k, v in proof.items() if k not in ('pre_capture_proof', 'verified_utc', 'verified_indexes')}, 'pre/post producer proof disagreement')
    retained = absolute(Path(proof['retained']))
    paths = {}
    for name, record in proof['files'].items():
        require(not Path(name).is_absolute() and '..' not in Path(name).parts and ':' not in name and '\\' not in name, 'retained proof path')
        paths[name] = retained / name
        require(identity(paths[name]) == record['identity'], 'retained producer proof drift: ' + name)
    require(proof['sources'] == {name: proof['files'][name]['identity'] for name in proof['sources']} and
            set(proof['sources']) == {'probes/saturation/' + name for name in SOURCE_NAMES} | {'CMakeLists.txt'} and
            proof['image'] == proof['files']['image.exe']['identity'], 'source/image proof binding')
    validate_build_proof(proof, paths)
    return proof


def check_identity(packet, proof):
    require(packet['schema'] == COHORT_SCHEMA and packet['status'] == 'INCOMPLETE_QUALITY' and
            packet['cohort'] in NAMES + SPLIT_NAMES[3:] and packet['diagnostic_factor'] in (2, 4, 8), 'cohort identity/status')
    require(packet['consumer'] == proof['consumer'] and packet['configuration'] == proof['configuration'] and
            packet['image'] == proof['image'] and packet['sources'] == proof['sources'], 'mixed producer image/configuration/source closure')
    require(packet['selected_factor'] is None and packet['adoptable'] is False and
            packet['production'] == 'FORCED_BYPASS' and packet['cpu_validation'] == 'DEFERRED', 'cohort cannot qualify/adopt factor')


def capture_window(directory, proof, end):
    start = datetime.datetime.fromisoformat(proof['created_utc'])
    require(start.tzinfo is not None and end.tzinfo is not None and start <= end, 'producer proof UTC capture window')
    lower, upper = start.timestamp(), end.timestamp()
    require(all(lower <= path.stat().st_mtime <= upper for path in directory.iterdir()),
            'stale cohort or artifact outside pre/post capture proof window')


def verify_proof(source, output, directories):
    absolute(output); require(not output.exists() and directories, 'fresh post-capture proof and cohorts required')
    proof = read_proof(source, verified=False)
    for name, record in proof['files'].items():
        require(identity(absolute(Path(record['original']))) == record['identity'], 'source/build/image changed after capture: ' + name)
    indexes = {}
    verified_time = datetime.datetime.now(datetime.timezone.utc)
    for directory in directories:
        absolute(directory); artifacts(directory)
        capture_window(directory, proof, verified_time)
        check_identity(load_json(directory / 'cohort.json'), proof)
        indexes[str(directory)] = identity(directory / 'artifact-hashes.csv')
    require(len(indexes) == len(directories), 'duplicate post-capture cohort directory')
    proof['pre_capture_proof'] = dict(path=str(source), identity=identity(source))
    proof['verified_utc'] = verified_time.isoformat()
    proof['verified_indexes'] = indexes
    write_json(output, proof)
    return proof


def compare_gates(stored, calculated):
    require(len(stored) == len(calculated) and len({g['name'] for g in stored}) == len(stored), 'missing/extra/duplicate gates')
    require(all(type(g['pass']) is bool and type(g['invalid']) is bool for g in stored), 'gate booleans required')
    require({g['name']: (g['pass'], g['invalid']) for g in stored} ==
            {g['name']: (g['pass'], g['invalid']) for g in calculated}, 'forged gate evidence disagrees with metrics')


def finalize(output, directories, source):
    absolute(output); absolute(source)
    require(not output.exists() and len(directories) in (4, 6) and len(set(directories)) == len(directories) and
            all(p.is_absolute() for p in directories), 'exactly four or six distinct absolute cohorts and fresh output required')
    proof = read_proof(source)
    require(set(proof['verified_indexes']) == {str(directory) for directory in directories}, 'exact verified cohort union required')
    tables = {}; packets = {}; references = []; manifest_hash = None; factor = None
    for directory in directories:
        index = artifacts(directory); packet = load_json(directory / 'cohort.json'); check_identity(packet, proof)
        capture_window(directory, proof, datetime.datetime.fromisoformat(proof['verified_utc']))
        name = packet['cohort']; require(name not in packets, 'duplicate cohort')
        require(proof['verified_indexes'].get(str(directory)) == identity(directory / 'artifact-hashes.csv'), 'cohort lacks unchanged-source post-capture proof')
        current_manifest = identity(directory / 'frozen-manifest.json')['sha256']
        require(packet['frozen_manifest_sha256'] == current_manifest and (manifest_hash is None or manifest_hash == current_manifest), 'mixed frozen manifest')
        require(factor is None or factor == packet['diagnostic_factor'], 'mixed diagnostic factors')
        manifest_hash, factor = current_manifest, packet['diagnostic_factor']
        manifest = load_json(directory / 'frozen-manifest.json')
        require(manifest['factor'] == factor and manifest['record_frames'] == 65536 and
                manifest['frozen_input_vectors_sha256'] == identity(directory / 'frozen-input-vectors.csv')['sha256'], 'frozen workload binding')
        validate_input_vectors(read_csv(directory / 'frozen-input-vectors.csv'))
        require([record['file'] for record in manifest['sources']] == list(SOURCE_NAMES[:8]), 'original frozen source inventory')
        for record in manifest['sources']:
            require(record['sha256'] == proof['sources']['probes/saturation/' + record['file']]['sha256'], 'frozen source proof disagreement')
        selected = NONALIAS if name == 'nonalias' else ('aliases.csv', 'alias-observations.csv', 'quality-observations.csv')
        if name != 'nonalias': scope = alias_scope(name)
        expected_members = ['calibration', 'neutral', 'compensation', 'characters', 'denseStateAndBursts', 'operatingGrid'] if name == 'nonalias' else [list(k) for k in alias_keys(*scope)]
        require(packet['member_keys'] == expected_members, 'exact cohort member keys')
        local = {filename: read_csv(directory / filename) for filename in selected}
        for filename in selected:
            if name == 'nonalias' or filename != 'quality-observations.csv': tables.setdefault(filename, []).extend(local[filename])
        expected_files = set(selected) | {'cohort.json', 'recordings.csv', 'frozen-manifest.json', 'frozen-input-vectors.csv'}
        recordings = read_csv(directory / 'recordings.csv')
        filenames = [r['file'] for r in recordings]
        expected_recordings = recording_spec(name, local)
        require(len(filenames) == len(set(filenames)) and set(filenames) == set(expected_recordings) and
                len(recordings) == (1402 if name == 'nonalias' else 4 * len(expected_members)), 'complete original recording inventory')
        for record in recordings:
            raw = record['file']; require(raw in index and raw.endswith('.f64'), 'raw recording missing')
            require(record['sha256'] == index[raw]['sha256'] and int(index[raw]['bytes']) ==
                    int(record['frames']) * int(record['channels']) * 8 and record['channels'] == '1' and
                    record['encoding'] == 'IEEE754-f64-little-endian', 'raw recording hash/size/format')
            require((number(record, 'sample_rate'), number(record, 'frames'), number(record, 'warmup_frames')) ==
                    expected_recordings[raw] and record['input'] and record['settings'], 'original raw recording extent/metadata')
            expected_files.add(raw)
        require(set(index) == expected_files, 'unknown/missing cohort artifacts')
        if name == 'nonalias': calculated = nonalias_gates(tables)
        else:
            calculated = [calibration_gate(local['quality-observations.csv'])] + alias_gates(local['aliases.csv'], local['alias-observations.csv'], *scope)
            require(calculated[0]['pass'] and not calculated[0]['invalid'], 'alias cohort calibration failed; native capture cannot complete')
        compare_gates(packet['gates'], calculated)
        packets[name] = packet
        references.append(dict(cohort=name, directory=str(directory), artifact_index=identity(directory / 'artifact-hashes.csv')))
    require(set(packets) in (set(NAMES), set(SPLIT_NAMES)), 'missing/unknown quality cohort')
    gates = quality_gates(tables)
    invalid = any(g['invalid'] for g in gates); passed = all(g['pass'] for g in gates)
    decision = dict(schema=1, scope='quality_only', diagnostic_factor=factor,
        status='invalid' if invalid else 'quality_pass' if passed else 'quality_rejected',
        quality_gates_pass=passed and not invalid, expected_quality_gates=22, selected_factor=None,
        adoptable=False, production='FORCED_BYPASS', cpu_validation='DEFERRED',
        not_run={name: 'NOT_RUN' for name in ('core_cpu_mean_and_p99', 'callback_cpp_heap', 'core_cpu_output_integrity')},
        frozen_manifest_sha256=manifest_hash, gates=gates, cohorts=references,
        source_proof=dict(path=str(source), identity=identity(source)), routing='inherited parent model; no overrides')
    output.mkdir(parents=True)
    write_json(output / 'decision.json', decision)
    return 2 if invalid else 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--prepare-proof', type=Path)
    mode.add_argument('--verify-proof', type=Path)
    mode.add_argument('--finalize', type=Path)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--consumer')
    parser.add_argument('--output-proof', type=Path)
    parser.add_argument('--cohorts', type=Path, nargs='+')
    parser.add_argument('--source-proof', type=Path)
    args = parser.parse_args()
    try:
        if args.prepare_proof:
            require(args.build and args.source_root and args.consumer and not any((args.cohorts, args.source_proof, args.output_proof)), 'prepare-proof arguments')
            prepare_proof(args.prepare_proof, args.build, args.consumer, args.source_root)
            return 0
        if args.verify_proof:
            require(args.output_proof and args.cohorts and not any((args.build, args.source_root, args.consumer, args.source_proof)), 'verify-proof arguments')
            verify_proof(args.verify_proof, args.output_proof, args.cohorts)
            return 0
        require(args.cohorts and args.source_proof and not any((args.build, args.source_root, args.consumer, args.output_proof)), 'finalize arguments')
        return finalize(args.finalize, args.cohorts, args.source_proof)
    except (ValueError, OSError, KeyError, TypeError, csv.Error, OverflowError) as error:
        print('INVALID/incomplete quality cohorts: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
