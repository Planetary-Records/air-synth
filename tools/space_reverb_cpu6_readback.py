"""Read every frozen periodic probe record after the exclusive trial has stopped."""
import argparse
import hashlib
import json
import math
import re
import struct
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def fields(line):
    return dict(re.findall(r"(\S+)=(\S+)", line))


def statistics(values):
    ordered = sorted(values)
    return dict(mean=math.fsum(values) / len(values), p99=ordered[math.ceil(.99 * len(values)) - 1],
                maximum=ordered[-1], minimum=ordered[0]) if values else dict(mean=0, p99=0, maximum=0, minimum=0)


def near(first, second):
    return math.isclose(float(first), float(second), rel_tol=1e-10, abs_tol=1e-15)


def audit(evidence):
    manifest = read(evidence / "manifest.json")
    assert manifest["admission_protocol"] == "periodic-proaudio6" and manifest["rows"] == 12
    assert manifest["stable_idle_seconds"] == 15 and manifest["workload_order"] == "tank_major"
    assert manifest["core_mean_p99_fraction"] == .5 and manifest["copy_p99_fraction"] == .1
    assert manifest["deadline"] == "scheduled_release_plus_period" and manifest["no_skips"]
    source_paths = {item["path"].replace("\\", "/") for item in manifest["source"]}
    assert {"source/" + p for p in (
        "probes/space-reverb/SpaceReverbHandoffProbe.cpp", "probes/space-reverb/HandoffBlockRender.h",
        "probes/space-reverb/ProbePeerGuard.h", "probes/space-reverb/ProbePeerMonitor.h",
        "probes/space-reverb/PeriodicCaller.h", "probes/space-reverb/PeriodicCaller.cpp",
        "src/dsp/SpaceReverbTank.cpp", "src/dsp/SpaceReverbLoss.cpp", "src/dsp/SpaceReverbTail.cpp",
        "src/dsp/SpaceReverbPacked.cpp", "src/dsp/SpaceReverbAvx2.cpp", "src/dsp/SpaceReverbModel.cpp",
        "src/dsp/space/CMakeLists.txt", "probes/space-reverb/CMakeLists.txt")} <= source_paths
    assert sha(evidence / "RackSynthSpaceReverbHandoffCpuFrozen.exe") == manifest["binary_sha256"]
    assert sha(evidence / "controller.ps1") == manifest["controller_sha256"]
    for item in manifest["source"]:
        assert sha(evidence / item["path"]) == item["sha256"], item["path"]
    attempts = []
    completed_rows = set()
    for path in sorted(evidence.glob("row-*-attempt-*.json")):
        saved = read(path)
        prefix = path.with_suffix("")
        stdout, stderr = Path(str(prefix) + ".stdout"), Path(str(prefix) + ".stderr")
        assert sha(stdout) == saved["stdout_sha256"] and sha(stderr) == saved["stderr_sha256"]
        lines = stdout.read_text(encoding="utf-8-sig").splitlines()
        result = [line for line in lines if line.startswith("HANDOFF_CPU_RESULT ")]
        timing_path = Path(str(prefix) + ".timings.bin")
        cycle_path = Path(str(timing_path) + ".cycles.bin")
        schedule_path = Path(str(timing_path) + ".schedule.bin")
        if not result:
            assert saved["exit_code"] == 3 and not timing_path.exists() and not cycle_path.exists() and not schedule_path.exists()
            assert saved["timings_sha256"] is None and saved["cycles_sha256"] is None and saved["schedule_sha256"] is None
            assert len([s for s in lines if s.startswith("HANDOFF_CPU_ADMISSION_BLOCKED ")]) == 1
            attempts.append(dict(row=saved["row"], attempt=saved["attempt"], complete=False, passed=False,
                                 samples=0, misses=[], status="BLOCKED_BEFORE_MEASUREMENT", record_sha256=sha(path)))
            continue
        assert len(result) == 1
        f = fields(result[0])
        row = int(f["row"])
        assert row == saved["row"] and 0 <= row < 12
        rate, block = (44100, 48000, 96000)[row // 4], (64, 512)[row // 2 % 2]
        period = block / rate
        assert int(f["rate"]) == rate and int(f["block"]) == block and f["domain"] == "poleContour"
        assert f["case"] == ("full_size_bank_change" if row % 2 else "geometry_grow_shrink")
        assert f["backend"] in ("avx2", "sse2") and near(f["period_seconds"], period)
        assert near(f["limit_seconds"], .5 * period) and near(f["copy_limit_seconds"], .1 * period)
        for p, key, size_key in ((timing_path, "timings_sha256", "timings_bytes"),
                                 (cycle_path, "cycles_sha256", "cycles_bytes"),
                                 (schedule_path, "schedule_sha256", "schedule_bytes")):
            assert sha(p) == saved[key] and p.stat().st_size == saved[size_key]
        raw, cycle_raw, schedule_raw = timing_path.read_bytes(), cycle_path.read_bytes(), schedule_path.read_bytes()
        assert len(raw) % 24 == 0 and len(cycle_raw) == len(raw) // 24 * 8 and len(schedule_raw) == len(raw)
        records = list(struct.iter_unpack("<ddQ", raw))
        cycles = [v[0] for v in struct.iter_unpack("<Q", cycle_raw)]
        schedules = list(struct.iter_unpack("<qqq", schedule_raw))
        assert int(f["samples"]) == len(records)
        metadata = [fields(s) for s in lines if s.startswith("HANDOFF_PERIODIC_CALLER ")]
        assert len(metadata) == 1
        p = metadata[0]
        assert p["mode"] == "periodic-proaudio6" and p["opened"] == p["closed"] == "1"
        assert p["task"] == "Pro_Audio" and p["timer"] == "high_resolution" and p["no_skips"] == "1"
        assert p["deadline"] == "scheduled_release_plus_period" and p["ticks_per_second"] == "1000000000"
        assert p["priority_before"] == p["priority_after"] and p["cleanup_error"] == "0"
        expected = (20 * rate + block - 1) // block
        assert int(p["expected_samples"]) == expected and int(p["warm_callbacks"]) == (10 * rate + block - 1) // block
        assert int(p["samples"]) == len(records) <= expected and p["schedule_valid"] == "1"
        anchor, tick_rate = int(p["anchor_tick"]), int(p["ticks_per_second"])
        assert not records or anchor > 0
        late_indices, start_lateness, completion_elapsed = [], [], []
        for ordinal, ((seconds, copy_seconds, copy_bytes), (release, start, finish)) in enumerate(zip(records, schedules)):
            assert math.isfinite(seconds) and seconds >= 0 and math.isfinite(copy_seconds) and copy_seconds >= 0
            assert copy_seconds <= seconds + 1e-15
            assert copy_bytes % 8 == 0 and copy_bytes <= int(f["prepared_bytes"])
            assert copy_bytes or copy_seconds == 0
            assert release == anchor + (ordinal + 1) * block * tick_rate // rate
            deadline = anchor + (ordinal + 2) * block * tick_rate // rate
            assert start >= release and finish >= start and (ordinal == 0 or start >= schedules[ordinal - 1][2])
            assert near(seconds, (finish - start) / tick_rate)
            start_lateness.append((start - release) / tick_rate)
            completion_elapsed.append((finish - release) / tick_rate)
            if finish > deadline:
                late_indices.append(ordinal)
        values = [r[0] for r in records]
        copies = [r[1] for r in records if r[2]]
        actual, copy_stats = statistics(values), statistics(copies)
        for key, native in (("mean", "mean_seconds"), ("p99", "p99_seconds"), ("maximum", "max_seconds")):
            assert near(actual[key], f[native])
        for key, native in (("mean", "copy_mean_seconds"), ("p99", "copy_p99_seconds"), ("maximum", "copy_max_seconds")):
            assert near(copy_stats[key], f[native])
        assert int(f["copy_events"]) == len(copies)
        native_misses = [fields(s) for s in lines if s.startswith("HANDOFF_DEADLINE_MISS ")]
        assert [int(v["ordinal"]) for v in native_misses] == late_indices and int(f["misses"]) == len(late_indices)
        for native, ordinal in zip(native_misses, late_indices):
            assert int(native["row"]) == row and near(native["seconds"], completion_elapsed[ordinal])
            assert near(native["kernel_seconds"], records[ordinal][0])
        assert lines.count("HANDOFF_WORKLOAD order=tank_major scratch_bytes=151552") == 1
        assert lines.count("HANDOFF_ALLOCATION_DOMAIN render_thread=1") == 1
        assert lines.count(f"HANDOFF_CYCLE_DIAGNOSTIC enabled=1 valid=1 samples={len(records)} format=uint64_native_little_endian outside_block_timer=1") == 1
        monitor = [fields(s) for s in lines if s.startswith("HANDOFF_INVENTORY_MONITOR ")]
        assert len(monitor) == 1 and monitor[0]["mode"] == "worker" and int(monitor[0]["scans"]) > 0
        assert monitor[0]["cadence_ms"] == "100" and monitor[0]["freshness_ms"] == "250"
        assert math.isfinite(float(monitor[0]["maximum_scan_seconds"])) and float(monitor[0]["maximum_scan_seconds"]) >= 0
        complete = f["interrupted"] == "0"
        passed = (complete and len(records) == expected and p["failure"] == p["platform_error"] == "0"
                  and float(f["warm_seconds"]) >= 10 and float(f["measure_seconds"]) >= 20 and bool(records) and bool(copies)
                  and f["adopted"] == "1" and f["allocations"] == f["frees"] == f["overflow"] == "0"
                  and math.isfinite(float(f["checksum"])) and float(f["peak"]) < 4
                  and actual["mean"] <= .5 * period and actual["p99"] <= .5 * period
                  and copy_stats["p99"] <= .1 * period and not late_indices)
        assert passed == (f["pass"] == "1") and saved["exit_code"] == (0 if passed else 1 if complete else 3)
        assert lines.count(f"HANDOFF_CPU_FOOTER row={row} complete={int(complete)} acceptance=0") == 1
        if complete:
            assert row not in completed_rows
            completed_rows.add(row)
            c = read(evidence / f"row-{row:02d}.complete.json")
            assert c["attempt"] == saved["attempt"] and c["exit_code"] == saved["exit_code"] and c["valid_complete_native_record"]
            assert c["fields"] == f
            final_deadline = anchor + (expected + 1) * block * tick_rate // rate
            assert float(f["measure_seconds"]) + 1e-9 >= (final_deadline - anchor) / tick_rate
        misses = [dict(ordinal=i,scheduled_release_tick=schedules[i][0],start_tick=schedules[i][1],finish_tick=schedules[i][2],
                       elapsed_from_release=completion_elapsed[i],kernel_seconds=records[i][0],copy_seconds=records[i][1],
                       copy_upper_bytes=records[i][2],thread_cycles=cycles[i]) for i in late_indices]
        attempts.append(dict(row=row,attempt=saved["attempt"],complete=complete,passed=passed,samples=len(records),
                             interrupt_reason=f["interrupt_reason"],period_seconds=period,statistics=actual,copy_statistics=copy_stats,
                             start_lateness=statistics(start_lateness),completion_from_release=statistics(completion_elapsed),misses=misses,
                             metadata=p,monitor=monitor[0],record_sha256=sha(path),timings_sha256=sha(timing_path),
                             cycles_sha256=sha(cycle_path),schedule_sha256=sha(schedule_path)))
    complete = [a for a in attempts if a["complete"]]
    failed = [a["row"] for a in complete if not a["passed"]]
    partial_failed = [dict(row=a["row"],attempt=a["attempt"],misses=a["misses"]) for a in attempts if not a["complete"] and a["misses"]]
    status = "FAILED_KNOWN_PARTIAL_DEADLINE_GATE" if partial_failed else "FAILED_CPU_GATE" if failed else "PASS_SCOPED_PERIODIC_CONDITION" if len(complete) == 12 else "INCOMPLETE"
    return dict(status=status,expected_rows=12,completed_rows=len(complete),failed_rows=failed,partial_failures=partial_failed,
                all_frozen_and_ordered_raw_identities_match=True,total_measured_blocks=sum(a["samples"] for a in complete),
                all_attempt_measured_blocks=sum(a["samples"] for a in attempts),all_attempt_deadline_misses=sum(len(a["misses"]) for a in attempts),
                attempts=attempts,scope="Periodic Pro Audio tank/copy condition only; no normal-priority, actual-host, field/service or physical acceptance",
                prior="CPU1 through CPU5 remain retired failures",cycle_interpretation="Raw cycles only; no elapsed-time conversion or scheduling-cause attribution")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = audit(args.evidence)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps({k: report[k] for k in ("status", "completed_rows", "failed_rows", "all_attempt_measured_blocks", "all_attempt_deadline_misses")}))
