"""Independent actual-processor capacity readback; no native process is launched."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import struct

CONDITION = "space-product-capacity1-four-mixed-layers16-notes64-routes8-serial-space"
MATRIX = [(rate, block, higher, lifecycle) for higher in (False, True)
          for rate in (44100, 48000, 96000) for block in (64, 512)
          for lifecycle in (False, True)]
WORKLOAD_GEOMETRY = {(21, 2831): 2063, (23, 2983): 2215, (24, 3303): 2535, (25, 3303): 2535,
                     (26, 3503): 2735, (27, 3539): 2735, (28, 3659): 2735}


def require(good, message):
    if not good:
        raise ValueError(message)


def close(actual, expected):
    require(isinstance(actual, (float, int)) and not isinstance(actual, bool)
            and math.isfinite(actual) and math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-5),
            f"summary differs: {actual!r} versus {expected!r}")


def voices_valid(active, ordinal, rate, block, lifecycle):
    if type(active) is not int or not 0 <= active <= 16:
        return False
    end = (ordinal + 1) * block
    release, stop, retrigger = ((rate * part + 99) // 100 for part in (150, 165, 180))
    if not lifecycle or end <= release or end > retrigger:
        return active == 16
    if stop < end <= retrigger:
        return active == 0
    return True


def validate_case(row, geometry):
    rate, block, higher, lifecycle = geometry
    require((row["rate"], row["host_block"], row["higher_offline"], row["phase"])
            == (rate, block, higher, "lifecycle" if lifecycle else "steady"), "case geometry/order")
    require(row["condition"] == CONDITION, "case condition")
    require(row["selected_quality"] == row["effective_quality"] == int(higher)
            and row["quality_pending_initial"] is False and row["host_non_realtime"] is higher,
            "actual selected/effective quality")
    require(row["active_musical_voices_after_warm"] == 16, "warm musical voices")
    count = math.ceil((2 if lifecycle else 1) * rate / block)
    warm = math.ceil(rate / (4 * block))
    require(row["measured_callbacks_expected"] == row["measured_callbacks_completed"] == count,
            "incomplete measured callback extent")
    require(row["warm_callbacks_expected"] == row["warm_callbacks_completed"] == warm,
            "incomplete warm callback extent")
    require(row["warm_host_frames"] == warm * block and row["measured_host_frames"] == count * block,
            "host-frame extent")
    for flag in ("technical_integrity", "periodic_opened", "periodic_closed", "retained_sound_unchanged", "raw_finite"):
        require(row[flag] is True, flag)
    require(row["failure"] == "" and row["nonzero_output_samples"] > 0, "failed/silent case")
    frequency, anchor = row["clock_ticks_per_second"], row["measurement_anchor_tick"]
    require(type(frequency) is int and 0 < frequency <= 1_000_000_000
            and type(anchor) is int and anchor > 0, "clock identity")
    period = block * 1e9 / rate
    close(row["period_ns"], period)
    raw, schedules, dispatch, voices = (row[name] for name in
        ("process_block_wall_ns", "schedule_ticks", "dispatch_lateness_ns", "actual_active_musical_voices_after_callbacks"))
    require(len(raw) == len(schedules) == len(dispatch) == len(voices) == count, "short raw arrays")
    require(row["space_required_converter_backend"] == "mirrored_avx2"
            and row["space_required_converter_backend_enum"] == 4, "Space backend identity")
    states, frames = row["space_metadata_before_after"], row["space_physical_frames_before_after"]
    require(len(states) == len(frames) == count, "short actual Space observations")
    factor = 2 if higher else 1
    physical = block * factor
    start = row["space_warm_start_frames"]
    warm_end = row["space_warm_finish_frames"]
    require(len(start) == len(warm_end) == 8
            and all(type(value) is int and value >= 0 for value in start + warm_end), "invalid warm Space counters")
    require([end - begin for begin, end in zip(start, warm_end)] == [warm * physical] * 8,
            "warm did not render every physical Space frame")
    require(row["space_measured_start_frames"] == warm_end, "Space counters changed between warm/measure")
    prior = warm_end
    totals = [0] * 8
    full_callbacks = bypassed_callbacks = transition_callbacks = 0
    for ordinal, (metadata, observed) in enumerate(zip(states, frames)):
        require(len(metadata) == len(observed) == 2 and len(observed[0]) == len(observed[1]) == 8,
                "invalid actual Space record shape")
        for state in metadata:
            require(len(state) == 5 and state[0] is True and state[1:3] == [128, factor] and state[4] == 4
                    and type(state[3]) is int and state[3] in (0, 255), "actual ready/density/factor/mask/backend")
        require(observed[0] == prior and all(type(value) is int and value >= 0 for value in observed[0] + observed[1]),
                "Space counters reset, jumped or changed outside callback")
        delta = [end - begin for begin, end in zip(observed[0], observed[1])]
        totals = [total + value for total, value in zip(totals, delta)]
        require(len(set(delta)) == 1 and 0 <= delta[0] <= physical, "eight actual Space kernels advanced unequal frames")
        before_mask, after_mask = metadata[0][3], metadata[1][3]
        if before_mask != after_mask:
            transition_callbacks += 1
        elif before_mask == 255:
            full_callbacks += 1
        else:
            bypassed_callbacks += 1
        if before_mask == after_mask:
            require(delta == [physical if before_mask else 0] * 8, "active Space frames skipped/bypassed Space advanced")
        if not lifecycle:
            require(before_mask == after_mask == 255, "steady Space mask")
        else:
            bypass = ((rate + block - 1) // block) * block
            unbypass = (((rate * 125 + 99) // 100 + block - 1) // block) * block
            fade = math.ceil(.005 * rate) + block
            first, end = ordinal * block, (ordinal + 1) * block
            if end <= bypass or first >= unbypass + fade:
                require(before_mask == after_mask == 255, "Space disabled outside bypass event")
            elif first >= bypass + fade and end <= unbypass:
                require(before_mask == after_mask == 0, "Space bypass did not settle")
        prior = observed[1]
    require(row["space_measured_finish_frames"] == prior, "Space final counters differ from callbacks")
    require(row["space_total_measured_physical_frames"] == totals
            and row["space_callbacks_full_active"] == full_callbacks
            and row["space_callbacks_fully_bypassed"] == bypassed_callbacks
            and row["space_callbacks_mask_transition"] == transition_callbacks, "Space totals differ from callback observations")
    completion = []
    for ordinal, (ticks, elapsed, late, active) in enumerate(zip(schedules, raw, dispatch, voices)):
        require(len(ticks) == 3 and all(type(value) is int for value in ticks), "invalid clock record")
        release, start, finish = ticks
        require(release == anchor + (ordinal + 1) * frequency * block // rate
                and finish >= start >= release, "rational release/start/finish")
        close(elapsed, (finish - start) * 1e9 / frequency)
        close(late, (start - release) * 1e9 / frequency)
        if finish > anchor + (ordinal + 2) * frequency * block // rate:
            completion.append(ordinal)
        require(voices_valid(active, ordinal, rate, block, lifecycle),
                "held/retriggered voices lost or MIDI Stop did not clear voices")
    ordered = sorted(raw)
    mean = sum(raw) / count
    p99 = ordered[math.ceil(.99 * count) - 1]
    median = ordered[count // 2] if count % 2 else (ordered[count // 2 - 1] + ordered[count // 2]) / 2
    misses = [ordinal for ordinal, value in enumerate(raw) if value > period]
    for field, value in (("mean_ns", mean), ("median_ns", median), ("p99_ns", p99), ("max_ns", ordered[-1])):
        close(row[field], value)
    require(row["processing_deadline_miss_ordinals"] == misses
            and row["processing_deadline_miss_count"] == len(misses), "processing misses were omitted")
    require(row["release_completion_deadline_miss_ordinals"] == completion
            and row["release_completion_deadline_miss_count"] == len(completion), "release/deadline misses were omitted")
    targets = mean <= .75 * period and p99 <= period
    require(row["comparative_nominal_targets_met"] is targets, "nominal-target classification")
    audit = row["build_heap_audit"]
    require(type(audit) is bool, "heap availability type")
    heaps = row["callback_heap_by_ordinal"]
    fields = ("callback_heap_allocations", "callback_heap_frees", "warm_callback_allocations", "warm_callback_frees")
    if audit:
        require(row["heap_hook_calibrated"] is True and len(heaps) == count
                and all(pair == [0, 0] for pair in heaps) and all(row[field] == 0 for field in fields), "callback heap audit")
    else:
        require(row["heap_hook_calibrated"] is False and heaps == []
                and all(row[field] is None for field in fields), "Release cannot claim heap observation")
    trace = row["events"]
    require(len(trace) == (8 if lifecycle else 0), "event extent")
    if lifecycle:
        for ordinal, (event, hundredth) in enumerate(zip(trace, (25, 50, 75, 100, 125, 150, 165, 180))):
            requested = (rate * hundredth + 99) // 100
            actual = ((requested + block - 1) // block) * block if ordinal < 5 else requested
            require(event["requested_host_frame"] == requested and event["actual_host_frame"] == actual
                    and event["executed"] is True and event["offcallback_operation_ns"] >= 0, "lifecycle event moved/omitted")
    expected_class = ("DEBUG_HEAP_AUDIT_TIMING_DIAGNOSTIC" if audit else
                      "HIGHER_OFFLINE_CAPACITY_OBSERVATION" if higher else
                      "NORMAL_HISTORICAL_STRESS" if rate == 96000 and block == 64 else
                      "NORMAL_HISTORICAL_QUALIFIED_TARGET")
    require(row["acceptance_class"] == expected_class, "scope classification changed")
    return {"audit": audit, "targets": targets, "processing_misses": len(misses),
            "release_completion_misses": len(completion)}


def validate_workload_state(data, higher):
    require(len(data) >= 16 + 32, "truncated workload state")
    magic, version, count, flags = struct.unpack_from("<4sIII", data)
    require(magic == b"ARSI" and flags == 0 and (version, count) in WORKLOAD_GEOMETRY,
            "workload codec/count")
    require(len(data) >= 16 + count * 8 + 78 + 32, "truncated workload values/layout")
    require(hashlib.sha256(data[:-32]).digest() == data[-32:], "workload sound digest")
    values = [struct.unpack_from("<f", data, 16 + index * 8 + 4)[0] for index in range(count)]
    require(all(math.isfinite(value) and 0 <= value <= 1 for value in values), "invalid retained workload values")
    require(values[758] == int(higher) and values[757] == 0, "workload quality/raw output protection")
    layout = data[16 + count * 8:16 + count * 8 + 78]
    require(layout[:9] == bytes([1, 1, 1, 1, 0, 1, 2, 3, 4])
            and layout[9:33] == bytes([1, 16, 4] * 8)
            and layout[73:78] == bytes([0, 0, 0, 0, 8]), "actual four-layer/eight-serial-Space topology")
    for layer in range(4):
        require(values[12 + 4 * layer] == 1 and values[13 + 4 * layer] == layer % 2,
                "actual mixed analog/wavetable layers")
        for route in range(16):
            require(values[252 + 80 * layer + 5 * route] == 1
                    and values[252 + 80 * layer + 5 * route + 4] != .5, "actual nonzero modulation route missing")
    start = WORKLOAD_GEOMETRY[version, count]
    for slot in range(8):
        require(values[572 + 23 * slot] == 0, "initial Space bypass")
        for band in range(6):
            require(values[start + 96 * slot + 12 + 6 * band] == 1
                    and values[start + 96 * slot + 48 + 8 * band] == 1,
                    "actual retained decay/post band disabled")
    return hashlib.sha256(data).hexdigest()


def readback(directory, binary=None):
    root = Path(directory).resolve()
    report = json.loads((root / "report.json").read_text(encoding="utf-8-sig"))
    preregistration = json.loads((root / "preregistration.json").read_text(encoding="utf-8-sig"))
    for key, value in preregistration.items():
        require(report.get(key) == value, f"preregistered field changed: {key}")
    require(report["condition"] == CONDITION and report["case_count_expected"] == report["case_count_completed"] == 24,
            "wrong/incomplete full matrix")
    require(report["mean_callback_target_fraction"] == .75 and report["p99_callback_target_fraction"] == 1,
            "declared comparison budgets changed")
    require(re.fullmatch(r"[0-9a-f]{64}", report["helper_sha256"]), "helper digest")
    if binary is not None:
        require(hashlib.sha256(Path(binary).read_bytes()).hexdigest() == report["helper_sha256"], "actual executable differs")
    hashes = {}
    for higher in (False, True):
        data = (root / ("higher-offline.soundstate" if higher else "normal.soundstate")).read_bytes()
        hashes[higher] = validate_workload_state(data, higher)
    require(len(report["cases"]) == 24, "case file count")
    accepted, all_targets, audit, processing, completion = True, True, None, 0, 0
    for ordinal, (entry, geometry) in enumerate(zip(report["cases"], MATRIX), 1):
        require(entry["file"] == f"case-{ordinal:02}.json", "duplicate/reordered/foreign case file")
        row = json.loads((root / entry["file"]).read_text(encoding="utf-8-sig"))
        require(row["workload_sound_sha256"] == hashes[geometry[2]], "workload differs across geometry")
        result = validate_case(row, geometry)
        require(entry["technical_integrity"] is True
                and entry["comparative_nominal_targets_met"] is result["targets"], "index differs from raw row")
        require(audit is None or audit == result["audit"], "mixed binary audit modes")
        audit = result["audit"]
        if not audit and not geometry[2] and geometry[:2] != (96000, 64):
            accepted = accepted and result["targets"]
        all_targets = all_targets and result["targets"]
        processing += result["processing_misses"]
        completion += result["release_completion_misses"]
    require(report["technical_integrity"] is True and report["all_comparative_nominal_targets_met"] is all_targets,
            "full summary differs")
    require(report["allocation_verified"] is audit
            and report["required_normal_targets_met"] == (None if audit else accepted), "audit/target summary differs")
    expected = "PASS_ACTUAL_PRODUCT_HEAP_AUDIT" if audit else "PASS_DECLARED_NORMAL_PRODUCT_CAPACITY_SCOPE" if accepted else "NOT_PASS"
    require(report["status"] == expected, "status differs from raw observations")
    return {"status": expected, "cases": 24, "all_nominal_targets_met": all_targets,
            "processing_deadline_misses": processing, "release_completion_deadline_misses": completion,
            "heap_audit": audit, "actual_binary_matched": binary is not None}


def require_all_realtime(result):
    require(result["cases"] == 24 and result["heap_audit"] is False,
            "all-geometry capacity requires a complete Release matrix")
    require(result["all_nominal_targets_met"] is True
            and result["processing_deadline_misses"] == 0
            and result["release_completion_deadline_misses"] == 0,
            "all-geometry capacity has processing or completion misses")


def self_test():
    # Synthetic tooling fixtures, never native product evidence.
    rate, block = 44100, 512
    count, warm, frequency, anchor = math.ceil(rate / block), math.ceil(rate / (4 * block)), 10_000_000, 100_000
    schedules = [[anchor + (n + 1) * frequency * block // rate,
                  anchor + (n + 1) * frequency * block // rate + 1,
                  anchor + (n + 1) * frequency * block // rate + 4] for n in range(count)]
    row = {"condition": CONDITION, "rate": rate, "host_block": block, "higher_offline": False, "phase": "steady",
           "selected_quality": 0, "effective_quality": 0, "quality_pending_initial": False,
           "host_non_realtime": False, "active_musical_voices_after_warm": 16,
           "measured_callbacks_expected": count, "measured_callbacks_completed": count,
           "warm_callbacks_expected": warm, "warm_callbacks_completed": warm,
           "warm_host_frames": warm * block, "measured_host_frames": count * block,
           "technical_integrity": True, "periodic_opened": True, "periodic_closed": True,
           "retained_sound_unchanged": True, "raw_finite": True, "failure": "", "nonzero_output_samples": 1,
           "clock_ticks_per_second": frequency, "measurement_anchor_tick": anchor, "period_ns": block * 1e9 / rate,
           "process_block_wall_ns": [300.] * count, "schedule_ticks": schedules, "dispatch_lateness_ns": [100.] * count,
           "actual_active_musical_voices_after_callbacks": [16] * count, "mean_ns": 300., "median_ns": 300., "p99_ns": 300., "max_ns": 300.,
           "processing_deadline_miss_ordinals": [], "processing_deadline_miss_count": 0,
           "release_completion_deadline_miss_ordinals": [], "release_completion_deadline_miss_count": 0,
           "comparative_nominal_targets_met": True, "build_heap_audit": False, "heap_hook_calibrated": False,
           "callback_heap_by_ordinal": [], "callback_heap_allocations": None, "callback_heap_frees": None,
           "warm_callback_allocations": None, "warm_callback_frees": None, "events": [],
           "acceptance_class": "NORMAL_HISTORICAL_QUALIFIED_TARGET"}
    warm_end = warm * block
    row.update({"space_required_converter_backend": "mirrored_avx2", "space_required_converter_backend_enum": 4,
                "space_metadata_before_after": [[[True, 128, 1, 255, 4], [True, 128, 1, 255, 4]] for _ in range(count)],
                "space_physical_frames_before_after": [[[warm_end + n * block] * 8, [warm_end + (n + 1) * block] * 8] for n in range(count)],
                "space_warm_start_frames": [0] * 8, "space_warm_finish_frames": [warm_end] * 8,
                "space_measured_start_frames": [warm_end] * 8, "space_measured_finish_frames": [warm_end + count * block] * 8,
                "space_total_measured_physical_frames": [count * block] * 8,
                "space_callbacks_full_active": count, "space_callbacks_fully_bypassed": 0, "space_callbacks_mask_transition": 0})
    validate_case(row, (rate, block, False, False))
    mutants = []
    wrong = copy.deepcopy(row); wrong["p99_ns"] = 299.; mutants.append(wrong)
    wrong = copy.deepcopy(row); wrong["schedule_ticks"][1][0] += 1; mutants.append(wrong)
    wrong = copy.deepcopy(row); wrong["process_block_wall_ns"].pop(); mutants.append(wrong)
    wrong = copy.deepcopy(row); wrong["processing_deadline_miss_ordinals"] = [0]; mutants.append(wrong)
    wrong = copy.deepcopy(row); wrong["callback_heap_allocations"] = 0; mutants.append(wrong)
    wrong = copy.deepcopy(row); wrong["actual_active_musical_voices_after_callbacks"][0] = 0; mutants.append(wrong)
    wrong = copy.deepcopy(row); wrong["space_physical_frames_before_after"][0][1][7] -= 1; mutants.append(wrong)
    wrong = copy.deepcopy(row); wrong["space_metadata_before_after"][0][1][4] = 2; mutants.append(wrong)
    wrong = copy.deepcopy(row); wrong["space_warm_finish_frames"][0] -= 1; mutants.append(wrong)
    for mutant in mutants:
        try:
            validate_case(mutant, (rate, block, False, False))
        except ValueError:
            pass
        else:
            raise ValueError("synthetic corruption was accepted")
    voice_checks = [(16, 0, True), (0, 0, False), (16, 561, True), (0, 561, False),
                    (8, 1150, True), (0, 1240, True), (16, 1240, False),
                    (16, 1350, True), (0, 1350, False)]
    for active, ordinal, expected in voice_checks:
        require(voices_valid(active, ordinal, 48000, 64, True) is expected,
                "synthetic lifecycle voice predicate differs")
    return {"status": "PASS_SYNTHETIC_READER_SELF_TEST", "native_evidence": False, "checks": 1 + len(mutants) + len(voice_checks)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", nargs="?")
    parser.add_argument("--binary")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--require-all-realtime", action="store_true")
    args = parser.parse_args()
    try:
        result = self_test() if args.self_test else readback(args.directory, args.binary)
        if args.require_all_realtime:
            require_all_realtime(result)
            result["status"] = "PASS_ALL24_ZERO_PROCESSING_AND_COMPLETION_MISSES"
        print(json.dumps(result, indent=2))
        raise SystemExit(1 if result["status"] == "NOT_PASS" else 0)
    except (ValueError, TypeError, KeyError, OSError, struct.error) as error:
        print(json.dumps({"status": "NOT_PASS", "reason": str(error)}, indent=2))
        raise SystemExit(1)
