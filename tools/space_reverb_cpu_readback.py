"""Independently verify frozen native CPU rows from their ordered binary timings."""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import re
from pathlib import Path
import sys


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def assess(root: Path) -> dict:
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig")) if manifest_path.exists() else {}
    policy = manifest.get("backend_policy", "sse2")
    rows = []
    for index in range(24):
        complete = root / f"row-{index:02d}.complete.json"
        if not complete.exists():
            rows.append({"row": index, "status": "INCOMPLETE"})
            continue
        record = json.loads(complete.read_text(encoding="utf-8-sig"))
        attempt = root / f"row-{index:02d}-attempt-{record['attempt']}.json"
        provenance = json.loads(attempt.read_text(encoding="utf-8-sig"))
        fields = record["fields"]
        timing_path = root / record["timings"]
        values = array.array("d")
        with timing_path.open("rb") as stream:
            values.frombytes(stream.read())
        if sys.byteorder != "little":
            values.byteswap()
        errors = []
        def require(condition: bool, reason: str) -> None:
            if not condition:
                errors.append(reason)
        require(sha256(timing_path).upper() == provenance["timings_sha256"].upper(), "timing hash")
        require(sha256(root / record["stdout"]).upper() == provenance["stdout_sha256"].upper(), "stdout hash")
        require(record["valid_complete_native_record"] and record["row"] == index, "native completeness")
        require(len(values) == int(fields["samples"]) and len(values) > 0, "sample count")
        require(all(math.isfinite(x) and x >= 0 for x in values), "finite durations")
        if errors:
            rows.append({"row": index, "status": "INVALID", "errors": errors})
            continue
        ordered = sorted(values)
        mean = math.fsum(values) / len(values)
        p99 = ordered[math.ceil(.99 * len(values)) - 1]
        maximum = ordered[-1]
        native_lines = (root / record["stdout"]).read_text(encoding="utf-8-sig").splitlines()
        native_results = [line for line in native_lines if line.startswith(f"CPU_RESULT row={index} ")]
        require(len(native_results) == 1 and native_lines.count(f"CPU_FOOTER row={index} complete=1 acceptance=0") == 1, "original native completeness")
        if len(native_results) == 1:
            require(dict(re.findall(r"(\S+)=(\S+)", native_results[0])) == fields, "original native field consistency")
        rate = (44100, 48000, 96000)[index // 8]
        block = (64, 512)[index // 4 % 2]
        period = block / rate
        misses = sum(x > period for x in values)
        for name, actual in (("mean_seconds", mean), ("p99_seconds", p99), ("max_seconds", maximum), ("period_seconds", period), ("limit_seconds", .5 * period)):
            require(math.isclose(actual, float(fields[name]), rel_tol=5e-12, abs_tol=1e-15), name + " readback")
        require(misses == int(fields["misses"]), "miss count readback")
        require(float(fields["warm_seconds"]) >= 10 and float(fields["measure_seconds"]) >= 20, "window length")
        require(fields["domain"] == "poleContour" and fields["tanks"] == "8", "workload")
        require(int(fields["rate"]) == rate and int(fields["block"]) == block, "rate/block")
        expected_case = "six-alternating" if index // 2 % 2 else "base-2.000000-brightness-1.000000"
        expected_size = 1.0 if index % 2 else .5
        backend = fields.get("backend", "sse2")
        require(fields.get("case") == expected_case and float(fields.get("size", "nan")) == expected_size, "curve/size workload")
        require(policy in ("sse2", "automatic") and backend in (("sse2", "avx2") if policy == "automatic" else ("sse2",)), "backend workload")
        require(fields["allocations"] == fields["frees"] == fields["overflow"] == fields["interrupted"] == "0", "resources/interruption")
        require(math.isfinite(float(fields["checksum"])), "checksum")
        require(int(fields["prepared_bytes"]) <= 8 * 16 * 1024 * 1024, "memory")
        passing = not errors and mean <= .5 * period and p99 <= .5 * period and misses == 0
        require((record["exit_code"] == 0) == (fields["pass"] == "1") == passing, "native/readback verdict agreement")
        rows.append({"row": index, "status": "PASS" if passing and not errors else "FAIL", "rate": rate, "block": block, "case": expected_case, "size": expected_size, "backend": backend, "samples": len(values), "mean_seconds": mean, "p99_seconds": p99, "max_seconds": maximum, "misses": misses, "errors": errors, "complete_sha256": sha256(complete), "timings_sha256": sha256(timing_path)})
    retired = root / "admission-stop1.json"
    return {"status": "PASS" if not retired.exists() and all(r["status"] == "PASS" for r in rows) else "NOT_PASS", "rows": rows, "retired_for_admission_failure": retired.exists(), "scope": "Isolated stationary eight-tank feasibility; no product or listening acceptance."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = assess(args.evidence)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"status": result["status"], "rows": len(result["rows"]), "passed": sum(r["status"] == "PASS" for r in result["rows"]), "misses": sum(r.get("misses", 0) for r in result["rows"])}))
    sys.exit(0 if result["status"] == "PASS" else 1)
