"""Run the Python development suites for several interpreters at once, one process per test module."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import subprocess
import sys
import threading
import time


RAN = re.compile(r"^Ran (\d+) tests? in ", re.MULTILINE)
COUNTS = re.compile(r"(failures|errors|skipped|expected failures|unexpected successes)=(\d+)")
# unittest exits 5 when a module defines no tests; the serial discovery run never failed on that.
NO_TESTS = 5
SUMMARY = re.compile(r"^Ran (\d+) tests? in [^\r\n]+\r?\n(?:[ \t]*\r?\n)*(OK(?: \([^\r\n]*\))?|NO TESTS RAN)[ \t]*(?:\r?\n|\Z)")


def parse_result(text, returncode):
    ran = list(RAN.finditer(text))
    tail = text[ran[-1].start():] if ran else ""
    summary = SUMMARY.match(tail)
    counts = {key: int(value) for key, value in COUNTS.findall(summary.group(2) if summary else tail)}
    return {"ran": int(ran[-1].group(1)) if ran else 0, "skipped": counts.get("skipped", 0),
            "failures": counts.get("failures", 0), "errors": counts.get("errors", 0),
            "ok": bool(summary) and ((returncode == 0 and int(summary.group(1)) > 0
                                      and summary.group(2).startswith("OK")
                                      and not any(counts.get(k, 0) for k in ("failures", "errors", "unexpected successes")))
                                     or (returncode == NO_TESTS and summary.group(1) == "0"
                                         and summary.group(2) == "NO TESTS RAN")),
            "returncode": returncode}


def schedule(suites, modules, weights):
    """Longest modules first, alternating interpreters, so the slowest work starts earliest."""
    ordered = sorted(modules, key=lambda module: (-weights.get(module, 0.0), module))
    return [(label, module) for module in ordered for label in suites]


def run_module(source, label, interpreter, module, output_root):
    log = output_root / label / f"{module}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with log.open("w", encoding="utf-8", errors="replace") as stream:
        result = subprocess.run([interpreter, "-m", "unittest", "discover", "-s", "tests", "-p", f"{module}.py", "-v"],
                                cwd=source, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT)
    text = log.read_text(encoding="utf-8", errors="replace")
    return {"suite": label, "module": module, "seconds": round(time.monotonic() - started, 2),
            "log": str(log), **parse_result(text, result.returncode)}


def summarize(results, suites):
    summary = {}
    for label in suites:
        rows = [row for row in results if row["suite"] == label]
        summary[label] = {key: sum(row[key] for row in rows) for key in ("ran", "skipped", "failures", "errors")}
        summary[label]["modules"] = len(rows)
        summary[label]["failedModules"] = sorted(row["module"] for row in rows if not row["ok"])
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--suite", action="append", required=True, metavar="LABEL=INTERPRETER")
    parser.add_argument("--jobs", type=int, default=6, help="concurrent modules per interpreter")
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args(argv)
    suites = dict(item.split("=", 1) for item in args.suite)
    modules = sorted(path.stem for path in (args.source / "tests").glob("test_*.py"))
    if not modules:
        raise SystemExit("No test modules found")
    durations_path = args.output_root / "python-durations.json"
    try:
        weights = json.loads(durations_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        # First run: file size is a rough stand-in for the measured duration.
        weights = {module: float((args.source / "tests" / f"{module}.py").stat().st_size) for module in modules}
    lock = threading.Lock()
    results = []

    def task(item):
        label, module = item
        row = run_module(args.source, label, suites[label], module, args.output_root)
        with lock:
            results.append(row)
            print(f"PYTHON_MODULE {label} {module} {'ok' if row['ok'] else 'FAILED'} ran={row['ran']} "
                  f"skipped={row['skipped']} seconds={row['seconds']}", flush=True)
            if not row["ok"]:
                print(Path(row["log"]).read_text(encoding="utf-8", errors="replace"), flush=True)
        return row

    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=max(1, args.jobs) * len(suites)) as pool:
        list(pool.map(task, schedule(suites, modules, weights)))
    summary = summarize(results, suites)
    durations = {module: max(row["seconds"] for row in results if row["module"] == module) for module in modules}
    args.output_root.mkdir(parents=True, exist_ok=True)
    durations_path.write_text(json.dumps(durations, indent=1, sort_keys=True), encoding="utf-8")
    for label, row in summary.items():
        print(f"PYTHON_SUITE {label} modules={row['modules']} ran={row['ran']} skipped={row['skipped']} "
              f"failures={row['failures']} errors={row['errors']} failed_modules={','.join(row['failedModules']) or '-'}")
    print(f"PYTHON_SUITES wall_seconds={round(time.monotonic() - started, 1)}")
    return 1 if any(row["failedModules"] for row in summary.values()) else 0


if __name__ == "__main__":
    raise SystemExit(main())
