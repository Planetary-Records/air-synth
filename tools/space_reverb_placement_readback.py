"""Independent caller-only placement diagnostic reader; never capacity acceptance."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def integer(value, maximum=(1 << 64) - 1):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError('invalid unsigned integer')
    return value


def analyze(raw: bytes, placement: dict) -> dict:
    if len(raw) < 128 or (len(raw) - 128) % 112:
        raise ValueError('raw length')
    h = struct.unpack('<Q6I12Q', raw[:128])
    if (h[:7] != (0x3155504356525353, 1, 0, 44100, 64, 1, 0)
            or h[7:10] != (6891, 13782, 1000000000)
            or h[13] > h[7] or h[14] > h[8] or len(raw) != 128 + 112 * h[14]):
        raise ValueError('unexpected diagnostic raw header')
    if placement.get('version') != 1 or placement.get('diagnostic_only') is not True or placement.get('topology_error') != 0:
        raise ValueError('invalid diagnostic/topology status')
    if integer(placement.get('warm_completed')) != h[13] or integer(placement.get('measured')) != h[14]:
        raise ValueError('sidecar/raw counts differ')
    topology, ids = {}, set()
    for cpu in placement['cpu_sets']:
        ident = integer(cpu['id'], (1 << 32) - 1)
        key = (integer(cpu['group'], 65535), integer(cpu['index'], 63))
        integer(cpu['core'], 255)
        integer(cpu['flags'], 255)
        integer(cpu['efficiency'], 255)
        if ident in ids or key in topology:
            raise ValueError('duplicate topology entry')
        topology[key] = cpu
        ids.add(ident)
    if not topology:
        raise ValueError('empty topology')
    endpoints = placement['placements']
    if len(endpoints) != h[13] + h[14]:
        raise ValueError('missing/extra placement')
    groups, processors, start_counts, migrations, warm_groups = {}, {}, {}, 0, {}
    records = list(struct.iter_unpack('<14Q', raw[128:]))
    previous = 0
    for k, endpoint in enumerate(endpoints):
        if len(endpoint) != 6:
            raise ValueError('endpoint shape')
        for value in endpoint:
            integer(value)
        phase = int(k >= h[13])
        ordinal = k - h[13] if phase else k
        if endpoint[:2] != [phase, ordinal]:
            raise ValueError('endpoint order/phase')
        before, after = tuple(endpoint[2:4]), tuple(endpoint[4:6])
        if before not in topology or after not in topology:
            raise ValueError('unknown processor endpoint')
        if not phase:
            efficiency = str(topology[before]['efficiency'])
            warm_groups[efficiency] = warm_groups.get(efficiency, 0) + 1
            continue
        r = records[ordinal]
        if (not r[0] <= r[1] <= r[2] <= r[3] <= h[15] or r[1] < previous
                or r[0] != h[10] + (ordinal + 1) * h[9] * h[4] // h[3]
                or r[13] != 1 or r[5] > 1 or r[6] > 1 or r[7] > 1 or r[8] not in (8, 9)
                or not all(math.isfinite(struct.unpack('<d', struct.pack('<Q', r[z]))[0]) for z in (11, 12))):
            raise ValueError('invalid raw record')
        previous = r[3]
        processor = f'{before[0]}:{before[1]}'
        start_counts[processor] = start_counts.get(processor, 0) + 1
        if before != after:
            migrations += 1
            continue
        efficiency = str(topology[before]['efficiency'])
        groups.setdefault(efficiency, []).append((r[3] - r[1]) / h[9])
        processors.setdefault(processor, []).append((r[3] - r[1]) / h[9])
    def stats(buckets):
        result = {}
        for key, durations in sorted(buckets.items()):
            durations.sort()
            result[key] = {'count': len(durations), 'mean_seconds': sum(durations) / len(durations),
                           'p99_seconds': durations[(99 * len(durations) + 99) // 100 - 1],
                           'max_seconds': durations[-1]}
        return result
    return {'status': 'DIAGNOSTIC_ONLY', 'capacity_acceptance': False,
            'warm_callbacks': h[13], 'measured_callbacks': h[14], 'complete_measurement': h[14] == h[8],
            'endpoint_migrations': migrations, 'same_endpoint_efficiency': stats(groups),
            'same_endpoint_processor': stats(processors), 'measured_start_processor_counts': start_counts,
            'caller_observations': {key: placement[key] for key in ('process_power_before', 'ideal_before', 'affinity_before')},
            'warm_start_efficiency_counts': warm_groups, 'topology': list(topology.values()),
            'limitation': 'Endpoint equality cannot exclude migration inside a callback. Efficiency class is topology, not measured clock or effective QoS. No capacity acceptance.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw', type=Path)
    parser.add_argument('placement', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output already exists')
    raw = args.raw.read_bytes()
    sidecar = args.placement.read_bytes()
    result = analyze(raw, json.loads(sidecar))
    result['bindings'] = {str(args.raw): hashlib.sha256(raw).hexdigest().upper(),
                          str(args.placement): hashlib.sha256(sidecar).hexdigest().upper(),
                          __file__: hashlib.sha256(Path(__file__).read_bytes()).hexdigest().upper()}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: result[key] for key in ('status', 'capacity_acceptance', 'measured_callbacks', 'endpoint_migrations', 'same_endpoint_efficiency')}))


if __name__ == '__main__':
    main()
