"""Validate the single qualified v2 characterization; never generate expected data."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
import math
from pathlib import Path
import stat
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
LEGACY_DIR = ROOT/'probes/saturation/contracts/fixtures/candidate-20261021'
REVISION_DIR = ROOT/'probes/saturation/contracts/fixtures/candidate-20261023-stable-v2'
MANIFEST_PIN = '30fc96fc8b4e0f1e9708786e9cf6ae80af42212adb42f7105f4620c8e51cd588'
LEGACY_MANIFEST_PIN = 'e0d1592a45083191d3e3d14f2548ec4b032c5fed0e0109d14969859917953599'
PARTITIONS = (1, 17, 64, 127, 512)
CASES = tuple(f'settled-{rate}-{mode}-{character}.f64'
              for rate in (44100,48000,96000) for mode in (0,1) for character in (0,1,2)) + (
                  'event-characterization', 'serial-characterization')
CASE_FILES = dict(zip(CASES, (*CASES[:18], 'event-expected.f64', 'serial-expected.f64')))
STIMULI = {
    'input.f64': dict(bytes=32768, sha256='73aee579e8868a65cf6ce2792477680d544b3833e51731c8fada0115892ae6fb'),
    'settled-settings.csv': dict(bytes=2789, sha256='6ce200bcaf26930be84061e9ed63a91a6c209081a3940226828ee4aaec116254'),
    'events-settings.csv': dict(bytes=4161, sha256='a50d3f20fc565652a0720a1cc22dc6f4830d48a0cb86e7e6d2aa02e5f2d44225'),
    'serial-settings.csv': dict(bytes=1534, sha256='ea170f4c3aad2cec36ed96f88bb8424d2665458ac8d66f3fb990361ffaf808bc'),
    'serial-events.csv': dict(bytes=180, sha256='0b2b8a18a5c56cc2ebc727487c7afda1c32aac7fc0c3c254e0b836d63ba39f2e'),
}
FIELDS = ['case','partition','expected_file','actual_file','max_absolute','bit_differences','pass',
          'first_mismatch_frame','first_mismatch_channel','actual_bits','expected_bits']
STATE = dict(status='UNADOPTED', CPU='DEFERRED', selected_factor=None, adoptable=False,
             production='FORCED_BYPASS', numeric_gates='UNCHANGED',
             compatibility_target='REVISED_BY_USER_20261023')
QUALIFICATIONS = ('original_inventory_and_stimuli', 'paired_439_proofs', 'source_normal_link_fp',
                  'current_prior100_and_legacy_metrics', 'five_partition_equality',
                  'independent_first_return_rational_decimal_mp', 'unchanged_numerical_control_factory_quality')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(data):
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def read_regular(path, maximum):
    try:
        mode = path.lstat()
        require(stat.S_ISREG(mode.st_mode) and not path.is_symlink()
                and not path.is_junction(), 'regular unredirected file required: '+str(path))
        require(0 < mode.st_size <= maximum, 'bounded nonempty file required: '+str(path))
        data = path.read_bytes()
        require(len(data) == mode.st_size, 'file changed during read: '+str(path))
        return data
    except OSError as error:
        raise ValueError('cannot read '+str(path)) from error


def members(directory, names):
    try:
        require(directory.is_dir() and not directory.is_symlink() and not directory.is_junction(),
                'plain directory required')
        require({p.name for p in directory.iterdir()} == set(names), 'exact directory membership required')
    except OSError as error:
        raise ValueError('cannot enumerate directory') from error


def json_bytes(data):
    def unique(pairs):
        result = {}
        for key,value in pairs:
            require(key not in result, 'duplicate JSON member: '+key)
            result[key] = value
        return result
    try:
        value = json.loads(data.decode('utf-8'), object_pairs_hook=unique,
                           parse_constant=lambda value: require(False, 'nonfinite JSON: '+value))
        require(isinstance(value, dict), 'JSON object required')
        return value
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError('malformed UTF-8 JSON') from error


def audio(data):
    require(len(data) == 32768, 'complete 2048 stereo frames required')
    values = struct.unpack('<4096d', data)
    require(all(math.isfinite(v) for v in values), 'finite audio required')
    return values


def reference(record):
    require(type(record) is dict and set(record) == {'path','identity'} and
            type(record['path']) is str and bool(record['path']), 'evidence reference required')
    bound = record['identity']
    require(type(bound) is dict and set(bound) == {'bytes','sha256'} and
            type(bound['bytes']) is int and bound['bytes'] > 0 and
            type(bound['sha256']) is str and len(bound['sha256']) == 64 and
            all(c in '0123456789abcdef' for c in bound['sha256']), 'SHA256 identity required')


def load_revision(directory: Path) -> dict:
    """Validate committed metadata without consulting private producer paths."""
    directory = Path(directory)
    names = set(STIMULI) | set(CASE_FILES.values()) | {'producer-receipt.json','manifest.json'}
    members(directory, names)
    raw = read_regular(directory/'manifest.json', 65536)
    require(identity(raw)['sha256'] == MANIFEST_PIN, 'external manifest pin mismatch')
    m = json_bytes(raw)
    required = set(STATE) | {'schema','revision','predecessor_manifest_sha256','probe_factor','frames',
                           'channels','format','partitions','cases','stimuli','sources','producer',
                           'qualification','files','scope'}
    require(set(m) == required, 'exact revision manifest schema required')
    require(all(type(m[k]) is type(v) and m[k] == v for k,v in STATE.items()), 'unadopted state required')
    require(type(m['schema']) is int and m['schema'] == 2 and
            m['revision'] == 'candidate-20261023-stable-v2' and
            m['predecessor_manifest_sha256'] == LEGACY_MANIFEST_PIN, 'revision/predecessor required')
    require(all(type(m[k]) is int and m[k] == v for k,v in [('probe_factor',4),('frames',2048),('channels',2)])
            and m['format'] == 'IEEE754_f64_little_endian_interleaved_stereo' and
            m['partitions'] == list(PARTITIONS) and all(type(n) is int for n in m['partitions']) and
            m['cases'] == CASE_FILES and list(m['cases']) == list(CASES), 'exact recording contract required')
    require(m['scope'] == 'normal_untransitioned_4x_qualified_characterization' and m['stimuli'] == STIMULI,
            'original stimuli and qualified scope required')
    require(type(m['files']) is dict and set(m['files']) == names-{'manifest.json'}, '26 payload identities required')
    payloads = {}
    for name,bound in m['files'].items():
        reference(dict(path=name,identity=bound))
        data = read_regular(directory/name, 65536 if name.endswith('.json') else 32768)
        require(identity(data) == bound, 'payload identity mismatch: '+name)
        payloads[name] = data
    require(all(m['files'][name] == bound for name,bound in STIMULI.items()), 'immutable stimulus identity required')
    r = json_bytes(payloads['producer-receipt.json'])
    require(set(r) == set(STATE) | {'schema','revision','predecessor_manifest_sha256','sources','producer',
                                  'qualification','staging','limitations'}, 'exact receipt schema required')
    require(r['schema'] == 'saturation-fixture-revision-producer-v1' and
            all(type(r[k]) is type(m[k]) and r[k] == m[k] for k in
                (*STATE,'revision','predecessor_manifest_sha256','sources','producer','qualification')),
            'frozen receipt/manifest binding required')
    require(type(m['sources']) is dict and len(m['sources']) == 8, 'qualified historical source identities required')
    for name,bound in m['sources'].items(): reference(dict(path=name,identity=bound))
    producer = m['producer']
    require(type(producer) is dict and set(producer) == {'acceptance','command','receipt','image','source_proof',
                                                      'current_recordings','prior_recordings','profile',
                                                      'started_utc','ended_utc','exit_code','source_phase'} and
            producer['profile'] == 'Release /O2 /MD /std:c++20 /fp:precise; normal Candidate',
            'historical normal producer required')
    for key in ('acceptance','receipt','image','source_proof'): reference(producer[key])
    require(type(producer['command']) is list and all(type(x) is str for x in producer['command']) and
            producer['command'][-2:] == ['--output',producer['current_recordings']] and
            type(producer['prior_recordings']) is str and type(producer['exit_code']) is int and
            producer['exit_code'] == 1 and all(type(producer[k]) is str and '+00:00' in producer[k]
            for k in ('started_utc','ended_utc')) and type(producer['source_phase']) is str and
            bool(producer['source_phase']), 'exact historical command/phase required')
    q = m['qualification']
    require(type(q) is dict and set(q) == {'status','checks','result','input_proof','analyses','limitations'} and
            q['status'] == 'PASS_BEFORE_FREEZE' and q['checks'] == list(QUALIFICATIONS) and
            type(q['analyses']) is list and len(q['analyses']) == 2 and
            type(q['limitations']) is str and bool(q['limitations']), 'complete independent qualification required')
    for record in (q['result'],q['input_proof'],*q['analyses']): reference(record)
    staging = r['staging']
    require(type(staging) is dict and set(staging) == {'utc','command','helper'} and
            type(staging['utc']) is str and '+00:00' in staging['utc'] and
            type(staging['command']) is list and bool(staging['command']) and
            r['limitations'] == q['limitations'], 'separate staging phase required')
    reference(staging['helper'])
    expected = {case: payloads[name] for case,name in CASE_FILES.items()}
    for data in expected.values(): audio(data)
    audio(payloads['input.f64'])
    return dict(manifest=m, receipt=r, expected=expected, stimuli=dict(STIMULI))


def comparison(actual, expected):
    av, ev = audio(actual), audio(expected)
    ab, eb = struct.unpack('<4096Q',actual), struct.unpack('<4096Q',expected)
    maximum = max(abs(a-b) for a,b in zip(av,ev))
    different = sum(a != b for a,b in zip(ab,eb))
    first = ['', '', '', '']
    if different:
        i = next(i for i in range(4096) if ab[i] != eb[i])
        first = [str(i//2),str(i%2),f'{ab[i]:016x}',f'{eb[i]:016x}']
    return dict(max_absolute=maximum,bit_differences=different,
                passed=maximum <= 1e-12 and different == 0,first_mismatch=first)


def compare_recordings(revision: dict, directory: Path) -> dict:
    """Check the complete Diagnosis protocol, preserving its real legacy result."""
    directory = Path(directory)
    filenames = [f'{case}.partition-{p}.actual.f64' for p in PARTITIONS for case in CASES]
    members(directory, {*filenames,'comparisons.csv','diagnosis.json'})
    legacy_manifest_raw = read_regular(LEGACY_DIR/'manifest.json',32768)
    require(identity(legacy_manifest_raw)['sha256'] == LEGACY_MANIFEST_PIN, 'original manifest pin required')
    legacy_manifest = json_bytes(legacy_manifest_raw)
    legacy = {}
    for case,name in CASE_FILES.items():
        data = read_regular(LEGACY_DIR/name,32768)
        require(identity(data) == legacy_manifest['files'][name], 'original expected identity required')
        legacy[case] = data
    for name,bound in STIMULI.items():
        require(identity(read_regular(LEGACY_DIR/name,32768)) == bound, 'original stimulus changed')
    try:
        reader = csv.DictReader(io.StringIO(read_regular(directory/'comparisons.csv',65536).decode('utf-8')), strict=True)
        require(reader.fieldnames == FIELDS, 'exact comparison CSV schema required')
        rows = list(reader)
    except (UnicodeError,csv.Error) as error:
        raise ValueError('malformed comparison CSV') from error
    require(len(rows) == 100, '100 ordered comparisons required')
    old_rows, new_rows = [], []
    for row,(p,case),filename in zip(rows,((p,c) for p in PARTITIONS for c in CASES),filenames):
        require(set(row) == set(FIELDS) and all(type(v) is str for v in row.values()), 'complete CSV row required')
        require([row[k] for k in FIELDS[:4]] == [case,str(p),CASE_FILES[case],filename], 'ordered case/partition/filename required')
        actual = read_regular(directory/filename,32768)
        old = comparison(actual,legacy[case])
        try:
            require(math.isfinite(float(row['max_absolute'])) and float(row['max_absolute']) == old['max_absolute']
                    and row['bit_differences'] == str(old['bit_differences']) and
                    row['pass'] == str(old['passed']).lower() and
                    [row[k] for k in FIELDS[7:]] == old['first_mismatch'], 'recalculated legacy metrics/first mismatch required')
        except (ValueError,OverflowError) as error:
            raise ValueError('invalid legacy metrics: '+case) from error
        old_rows.append(dict(case=case,partition=p,**old))
        new_rows.append(dict(case=case,partition=p,**comparison(actual,revision['expected'][case])))
    passed = sum(row['passed'] for row in old_rows)
    expected_diagnosis = dict(status='fixture_match' if passed == 100 else 'fixture_mismatch',
                             scope='diagnostic_only',selected_factor=None,adoptable=False,
                             production='FORCED_BYPASS',cpu_validation='DEFERRED',comparisons=100,
                             frames=2048,channels=2,passed=passed,failed=100-passed)
    diagnosis = json_bytes(read_regular(directory/'diagnosis.json',32768))
    require(set(diagnosis) == set(expected_diagnosis) and
            all(type(diagnosis[k]) is type(v) and diagnosis[k] == v for k,v in expected_diagnosis.items()),
            'recalculated diagnosis summary required')
    revised_passed = sum(row['passed'] for row in new_rows)
    return dict(status='PASS' if revised_passed == 100 else 'FAIL',
                legacy=dict(passed=passed,failed=100-passed,rows=old_rows,native_exit_code=0 if passed == 100 else 1),
                revised=dict(passed=revised_passed,failed=100-revised_passed,rows=new_rows),
                scope='current_v2_fixture_compatibility_only',CPU='DEFERRED',selected_factor=None,
                adoptable=False,production='FORCED_BYPASS',native_acceptance='NOT_RUN_BY_COMPARISON')


def plain_path(path):
    require(path.is_absolute(), 'absolute path required')
    require(path == path.resolve(), 'canonical absolute path required')
    require(all(not p.is_symlink() and not p.is_junction() for p in (path,*path.parents)),
            'unredirected path required')


def input_snapshot(native, revision):
    """Bind current inputs separately from the historical golden producer."""
    legacy_raw = read_regular(LEGACY_DIR/'manifest.json',32768)
    require(identity(legacy_raw)['sha256'] == LEGACY_MANIFEST_PIN, 'original manifest pin required')
    legacy = json_bytes(legacy_raw)
    members(LEGACY_DIR, {*legacy['files'],'manifest.json'})
    paths = [native, Path(__file__).resolve(), ROOT/'CMakeLists.txt',
             *(ROOT/name for name in revision['manifest']['sources']),
             *(LEGACY_DIR/name for name in (*legacy['files'],'manifest.json')),
             *(REVISION_DIR/name for name in (*revision['manifest']['files'],'manifest.json'))]
    result = {str(p): identity(read_regular(p, 512*1024*1024 if p == native else 1024*1024)) for p in paths}
    for name,bound in legacy['files'].items():
        require(result[str(LEGACY_DIR/name)] == bound, 'original payload identity required: '+name)
    from saturation_gain_parity import fixture_producer_inputs
    result.update(fixture_producer_inputs(native,ROOT))
    return result


def output_boundary(native, output):
    for path in (native,output): plain_path(path)
    for protected in (LEGACY_DIR,REVISION_DIR,native):
        require(not output.is_relative_to(protected) and not protected.is_relative_to(output),
                'output must not overlap inputs')


def run_diagnosis(native: Path, output: Path) -> dict:
    """Run the unchanged normal Diagnosis once; retain proof even on refusal."""
    native, output = Path(native), Path(output)
    output_boundary(native,output)
    read_regular(native,512*1024*1024)
    require(not output.exists() and output.parent.is_dir(), 'new output with existing parent required')
    output.mkdir()
    argv = [str(native),'--output',str(output/'native')]
    result = dict(status='REFUSED',exit_code=2,native_acceptance='NOT_ACCEPTED',
                  scope='current_v2_fixture_compatibility_only',CPU='DEFERRED',
                  selected_factor=None,adoptable=False,production='FORCED_BYPASS',
                  manifest_pin=MANIFEST_PIN,producer=dict(argv=argv,cwd=str(ROOT),exit_code=None),
                  inputs_before={},inputs_after={})
    stdout, stderr = b'', b''
    try:
        revision = load_revision(REVISION_DIR)
        result['inputs_before'] = input_snapshot(native,revision)
        proof=native.parents[2]/'saturation-gain-runtime'/native.parent.name/'RackSynthSaturationFixtureDiagnosis.json'
        (output/'producer-proof.json').write_bytes(read_regular(proof,16*1024*1024))
        result['producer']['started_utc'] = datetime.now(timezone.utc).isoformat()
        (output/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        try:
            child = subprocess.run(argv,cwd=ROOT,capture_output=True,timeout=540)
            stdout, stderr = child.stdout, child.stderr
            result['producer']['exit_code'] = child.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr = error.stdout or b'', error.stderr or b''
            raise ValueError('native runtime limit exceeded') from error
        finally:
            result['producer']['ended_utc'] = datetime.now(timezone.utc).isoformat()
            (output/'stdout.raw').write_bytes(stdout)
            (output/'stderr.raw').write_bytes(stderr)
            # Rehash before parsing: changed private inputs cannot be accepted.
            try:
                result['inputs_after'] = input_snapshot(native,revision)
            except (ValueError,OSError) as error:
                raise ValueError('inputs changed or invalid after native run: '+str(error)) from error
        require(result['inputs_after'] == result['inputs_before'], 'inputs changed during native run')
        load_revision(REVISION_DIR)
        input_snapshot(native,revision)
        require(child.returncode in (0,1), 'native producer exit must be 0 or 1')
        compared = compare_recordings(revision,output/'native')
        require(child.returncode == compared['legacy']['native_exit_code'], 'actual native exit/legacy summary contradiction')
        result.update(compared)
        result.update(exit_code=0 if compared['status'] == 'PASS' else 1,
                      native_acceptance='PASS' if compared['status'] == 'PASS' else 'FAIL')
    except (ValueError,OSError) as error:
        result.update(status='REFUSED',exit_code=2,native_acceptance='NOT_ACCEPTED',error=str(error))
    (output/'stdout.raw').write_bytes(stdout)
    (output/'stderr.raw').write_bytes(stderr)
    result['producer'].update(stdout=identity(stdout),stderr=identity(stderr))
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__,allow_abbrev=False)
    parser.add_argument('--check-corpus',action='store_true')
    parser.add_argument('--native',type=Path)
    parser.add_argument('--output-root',type=Path)
    args = parser.parse_args()
    if args.check_corpus:
        if args.native or args.output_root: parser.error('--check-corpus cannot run a producer')
    elif not (args.native and args.output_root):
        parser.error('use --check-corpus or --native with --output-root')
    try:
        if args.check_corpus:
            load_revision(REVISION_DIR)
            print('CORPUS PASS candidate-20261023-stable-v2; native acceptance NOT_RUN; CPU DEFERRED')
            return 0
        output_boundary(args.native,args.output_root)
        require(args.output_root.is_relative_to(ROOT) and args.output_root != ROOT,
                'worktree-private output root required')
        args.output_root.mkdir(parents=True,exist_ok=True)
        require(args.output_root.is_dir(), 'regular output root required')
        output = args.output_root/('attempt-'+uuid.uuid4().hex)
        result = run_diagnosis(args.native,output)
        print(f"V2 {result['status']} receipt={output/'result.json'} CPU=DEFERRED")
        return result['exit_code']
    except (ValueError,OSError) as error:
        parser.exit(2,'REFUSED '+str(error)+'\n')


if __name__ == '__main__':
    raise SystemExit(main())
