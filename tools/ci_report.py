"""Summarize CTest's passing and failing launcher timings without publishing raw output."""
import argparse
import json
import os
from pathlib import Path
import re
import xml.etree.ElementTree as ET


TIMING = re.compile(r"NATIVE_TIMING wait_ms=(\d+) run_ms=(\d+) exit_code=(-?\d+)")


def read_junit(path):
    with Path(path).open("rb") as stream:
        data = stream.read(16 * 1024 * 1024 + 1)
    if len(data) > 16 * 1024 * 1024:
        raise ValueError("CTest report exceeds 16 MiB")
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        raise ValueError("Malformed CTest report") from error
    tests = []
    for case in root.iter("testcase"):
        status = "skipped" if case.find("skipped") is not None else (
            "failed" if case.find("failure") is not None or case.find("error") is not None else "passed")
        matches = TIMING.findall("\n".join(case.itertext()))
        # The outer launcher completes last; nested records must not inflate the totals.
        wait, run, code = map(int, matches[-1]) if matches else (None, None, None)
        tests.append({"name": case.get("name", "unnamed"), "status": status,
                      "elapsed_seconds": float(case.get("time", "0")),
                      "wait_seconds": wait / 1000 if wait is not None else None,
                      "run_seconds": run / 1000 if run is not None else None, "exit_code": code})
    if not tests:
        raise ValueError("CTest report contains no tests")
    return {"tests": tests, **{s: sum(t["status"] == s for t in tests) for s in ("passed", "failed", "skipped")},
            "aggregate_wait_seconds": sum(t["wait_seconds"] or 0 for t in tests),
            "aggregate_run_seconds": sum(t["run_seconds"] or 0 for t in tests),
            "unknown_timing_tests": [t["name"] for t in tests if t["wait_seconds"] is None and t["status"] != "skipped"]}


def markdown(report):
    lines = [f"Native tests: {report['passed']} passed, {report['failed']} failed, {report['skipped']} skipped.", "",
             f"Aggregate resource waiting: {report['aggregate_wait_seconds']:.2f} s; "
             f"aggregate execution: {report['aggregate_run_seconds']:.2f} s.",
             "These totals sum parallel tests; they are not wall-clock duration.", ""]
    if report["unknown_timing_tests"]:
        lines += [f"Timing unavailable for {len(report['unknown_timing_tests'])} tests; elapsed time includes any waiting.", ""]
    lines += ["| Test | Resource wait (s) | Execution (s) | Result |", "| --- | ---: | ---: | --- |"]
    for test in sorted(report["tests"], key=lambda t: t["wait_seconds"] or 0, reverse=True)[:10]:
        wait = f"{test['wait_seconds']:.2f}" if test["wait_seconds"] is not None else "unknown"
        run = f"{test['run_seconds']:.2f}" if test["run_seconds"] is not None else "unknown"
        lines.append(f"| {test['name'].replace('|', '/')} | {wait} | {run} | {test['status']} |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--junit", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = read_junit(args.junit)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    summary = markdown(report)
    args.output.with_suffix(".md").write_text(summary, encoding="utf-8")
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as stream:
            stream.write(summary)
    return int(report["failed"] > 0)


if __name__ == "__main__":
    raise SystemExit(main())
