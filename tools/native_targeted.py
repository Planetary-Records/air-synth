"""Build generated dependencies of explicitly selected CTest checks, then run them once."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

from ci_workspace import plain_path, writer


SCOPE = "FOCUSED_ONLY_NOT_RELEASE_ACCEPTANCE"


class CommandFailed(RuntimeError):
    pass


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def path_key(path):
    return str(Path(path).resolve()).casefold()


def read_cache(build):
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    return cache


def validate_cache(cache, source, build, config):
    if config not in ("Debug", "Release"):
        raise ValueError("Configuration must be exactly Debug or Release")
    if build.resolve() not in [(source / "build/windows-debug").resolve(), (source / "build/ci").resolve()]:
        raise ValueError("Use this source's existing build/windows-debug or build/ci tree")
    for key, expected in [("CMAKE_HOME_DIRECTORY", source), ("CMAKE_CACHEFILE_DIR", build)]:
        if path_key(cache.get(key, "")) != path_key(expected):
            raise ValueError(f"Wrong or stale {key}; do not reuse another source/build tree")
    if config not in cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";"):
        raise ValueError("Configuration is absent from the existing multi-config cache")


def validate_pattern(pattern):
    if not pattern or any(c in pattern for c in "\r\n\0"):
        raise ValueError("Supply a nonempty, single-line CTest regex")
    try:
        re.compile(pattern)
    except re.error as error:
        raise ValueError(f"Invalid test regex: {error}") from error


def properties(test):
    return {p["name"]: p["value"] for p in test.get("properties", [])}


def select_tests(inventory, selected, config, allow_timing=False, allow_physical_desktop=False):
    # CTest compacts its diagnostic backtrace graph for filtered inventories.
    inventory = [{k: v for k, v in t.items() if k != "backtrace"} for t in inventory]
    selected = [{k: v for k, v in t.items() if k != "backtrace"} for t in selected]
    names = [t.get("name") for t in inventory]
    if any(not isinstance(n, str) or not n or any(c in n for c in "\n\r\0") for n in names) or len(set(names)) != len(names):
        raise ValueError("Empty, duplicate or invalid CTest inventory names")
    if not selected or len({t.get("name") for t in selected}) != len(selected):
        raise ValueError("CTest regex selected zero tests or duplicate names")
    original = {t["name"]: t for t in inventory}
    for test in selected:
        if test.get("name") not in original or test != original[test["name"]]:
            raise ValueError("Selected CTest metadata differs from the complete inventory")
        if test.get("config") != config or not test.get("command"):
            raise ValueError("Missing executable command or wrong CTest configuration; configure/build metadata is stale")
        props = properties(test)
        if props.get("DISABLED"):
            raise ValueError(f"Selected test is disabled: {test['name']}")
        command = test["command"]
        launcher_options = command[:command.index("--")] if "--" in command else command
        if ("timing" in props.get("LABELS", []) or "--exclusive" in launcher_options) and not allow_timing:
            raise ValueError("Timing checks require explicit --allow-timing; CPU acceptance stays separately deferred")
        if ("--desktop" in launcher_options or "desktop" in props.get("RESOURCE_LOCK", [])) and not allow_physical_desktop:
            raise ValueError("Physical desktop checks require explicit --allow-physical-desktop")
    return sorted(selected, key=lambda t: t["name"])


def exact_pattern(selected):
    return "^(" + "|".join(re.escape(t["name"]) for t in selected) + ")$"


def load_artifacts(index, source, build, config, configure_started_ns):
    if index.stat().st_mtime_ns < configure_started_ns:
        raise ValueError("CMake File API index predates this configure")
    reply = index.parent
    data = json.loads(index.read_text(encoding="utf-8"))
    responses = data.get("reply", {}).get("client-native-targeted", {}).get("query.json", {}).get("responses", [])
    models = [r for r in responses if r.get("kind") == "codemodel" and r.get("version", {}).get("major") == 2]
    if len(models) != 1 or "jsonFile" not in models[0]:
        raise ValueError("Missing current CMake File API codemodel response")
    model = json.loads((reply / models[0]["jsonFile"]).read_text(encoding="utf-8"))
    for key, expected in [("source", source), ("build", build)]:
        if path_key(model.get("paths", {}).get(key, "")) != path_key(expected):
            raise ValueError(f"Wrong File API {key} path")
    configurations = [c for c in model["configurations"] if c["name"] == config]
    if len(configurations) != 1:
        raise ValueError("Missing or ambiguous File API configuration")
    artifacts = {}
    for target in configurations[0].get("targets", []):
        metadata = json.loads((reply / target["jsonFile"]).read_text(encoding="utf-8"))
        if metadata["name"] != target["name"]:
            raise ValueError("File API target identity differs from configuration")
        for artifact in metadata.get("artifacts", []):
            key = path_key(build / artifact["path"])
            artifacts.setdefault(key, []).append(target["name"])
    return artifacts


def infer_targets(tests, artifacts, build, external_programs):
    targets = set()
    external = {path_key(p) for p in external_programs}
    for test in tests:
        props = properties(test)
        values = list(test["command"])
        values += [item.split("=", 1)[1] for item in props.get("ENVIRONMENT", [])
                   if "=" in item and not item.startswith("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT=")]
        values += [item.split("=", 1)[1].split(":", 1)[1]
                   for item in props.get("ENVIRONMENT_MODIFICATION", []) if "=" in item and ":" in item.split("=", 1)[1]]
        for token in values:
            # CTest stores argv separately, including --option=absolute/path and environment values.
            value = token.split("=", 1)[1] if token.startswith("-") and "=" in token else token
            candidate = Path(value)
            if not candidate.is_absolute():
                if candidate.suffix.lower() in (".exe", ".dll", ".vst3", ".so", ".dylib"):
                    raise ValueError(f"Relative executable/artifact cannot be mapped reliably: {value}")
                continue
            key = path_key(candidate)
            if key in artifacts:
                matches = set(artifacts[key])
                if len(matches) != 1:
                    raise ValueError(f"Ambiguous target artifact: {value}")
                targets.update(matches)
            elif candidate.is_relative_to(build):
                raise ValueError(f"Unmapped build artifact or runtime directory: {value}; use full CI for this selection")
            elif candidate.suffix.lower() in (".exe", ".dll", ".vst3", ".so", ".dylib") and key not in external:
                raise ValueError(f"Unmapped external executable/artifact: {value}")
    if not targets:
        raise ValueError("No generated native targets found for selected commands")
    return sorted(targets)


def validate_junit(path, names, started_ns):
    if not path.is_file() or path.stat().st_mtime_ns < started_ns:
        raise ValueError("Fresh JUnit is missing or stale")
    try:
        cases = list(ET.parse(path).getroot().iter("testcase"))
    except ET.ParseError as error:
        raise ValueError("Malformed JUnit") from error
    observed = [c.get("name") for c in cases]
    if not cases or sorted(observed) != sorted(names) or len(set(observed)) != len(observed):
        raise ValueError("JUnit final membership is empty, duplicate, or differs from selected tests")
    if any(c.get("status") != "run" or any(c.find(tag) is not None for tag in ("failure", "error", "skipped")) for c in cases):
        raise ValueError("JUnit contains failed, skipped or non-run tests")
    return {"passingNames": observed, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def publish(state):
    if "receiptPath" in state:
        write_json(Path(state["receiptPath"]), state)


def run_command(argv, log, source, state):
    argv = [str(a) for a in argv]
    command = {"argv": argv, "log": str(log), "startedNs": time.time_ns(), "exitCode": None}
    state["commands"].append(command)
    publish(state)
    print("TARGETED_COMMAND " + json.dumps(argv), flush=True)
    start = time.monotonic()
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run(argv, cwd=source, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                encoding="utf-8", errors="replace", shell=False)
    command.update(exitCode=result.returncode, elapsedSeconds=time.monotonic() - start, finishedNs=time.time_ns())
    publish(state)
    if result.returncode:
        raise CommandFailed(f"Command exited {result.returncode}; see {log}")
    return log.read_text(encoding="utf-8", errors="replace")


def source_identity(source):
    def git(*argv):
        return subprocess.check_output(["git", "-C", str(source), *argv])
    changed = {}
    names = git("diff", "--name-only", "-z", "HEAD") + git("ls-files", "-o", "--exclude-standard", "-z")
    for name in names.decode("utf-8").split("\0"):
        if name:
            path = source / name
            changed[name] = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
    return {"revision": git("rev-parse", "HEAD").decode().strip(),
            "status": git("status", "--porcelain=v1", "-z").decode("utf-8"), "dirtyFiles": changed}


def ctest_argv(ctest, build, config, selected, output, parallel):
    return [str(ctest), "--test-dir", str(build), "-C", config, "--parallel", str(parallel),
            "--resource-spec-file", str(Path(__file__).resolve().parent / "ci-resources.json"),
            "--output-on-failure", "--output-log", str(output / "ctest.log"),
            "--output-junit", str(output / "ctest.xml"), "-R", exact_pattern(selected)]


def execute(args):
    source = plain_path(args.source)
    build = plain_path(source / args.build_root)
    output = args.output_root.resolve() / (time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex[:12])
    output.mkdir(parents=True, exist_ok=False)
    state = {"version": 1, "scope": SCOPE, "outcome": "IN_PROGRESS", "source": str(source),
             "buildRoot": str(build), "configuration": args.config, "requestedRegex": args.test_regex,
             "receiptPath": str(output / "focused-receipt.json"), "commands": [], "startedNs": time.time_ns()}
    publish(state)
    print(f"{SCOPE}\nTARGETED_OUTPUT {output}", flush=True)
    started = time.monotonic()
    resources = ExitStack()
    previous_machine = os.environ.get("AIRACK_NATIVE_REQUIRE_MACHINE")
    try:
        if os.environ.get("AIRACK_NATIVE_LEASE"):
            raise ValueError("Run targeted entrypoint outside an umbrella native lease; CTest owns per-test leases")
        validate_pattern(args.test_regex)
        if build not in (source / "build/windows-debug", source / "build/ci"):
            raise ValueError("Use this source's existing build/windows-debug or build/ci tree")
        resources.enter_context(writer(build))
        cache = read_cache(build)
        validate_cache(cache, source, build, args.config)
        cmake = Path(args.cmake or cache["CMAKE_COMMAND"]).resolve()
        ctest = cmake.with_name("ctest.exe" if os.name == "nt" else "ctest")
        if not cmake.is_file() or not ctest.is_file():
            raise ValueError("Configured CMake and sibling CTest must exist")
        native = args.native_runner.resolve()
        if not native.is_file():
            raise ValueError("Supply the existing bootstrapped RackSynthNativeRun with --native-runner")
        # Never rebuild a running executable; the coordinator uses machine resources from its private copy.
        launcher = output / "RackSynthNativeRun.exe"
        shutil.copy2(native, launcher)
        os.environ["AIRACK_NATIVE_REQUIRE_MACHINE"] = "1"
        resource_info = run_command([launcher, "--resource-info"], output / "resources.log", source, state)
        if "NATIVE_RESOURCES scope=machine" not in resource_info:
            raise ValueError("Machine-scoped native resources are required")
        state["sourceIdentity"] = source_identity(source)
        query = build / ".cmake/api/v1/query/client-native-targeted/query.json"
        query.parent.mkdir(parents=True, exist_ok=True)
        write_json(query, {"requests": [{"kind": "codemodel", "version": 2}]})
        configure_started = time.time_ns()
        run_command([launcher, "--", cmake, "-S", source, "-B", build], output / "configure.log", source, state)
        validate_cache(read_cache(build), source, build, args.config)
        indexes = list((build / ".cmake/api/v1/reply").glob("index-*.json"))
        if not indexes:
            raise ValueError("CMake did not return File API metadata")
        index = max(indexes, key=lambda p: p.stat().st_mtime_ns)
        artifacts = load_artifacts(index, source, build, args.config, configure_started)
        base = [ctest, "--test-dir", build, "-C", args.config, "--show-only=json-v1"]
        inventory = json.loads(run_command(base, output / "inventory.json", source, state))
        matched = json.loads(run_command(base + ["-R", args.test_regex], output / "selected.json", source, state))
        selected = select_tests(inventory["tests"], matched["tests"], args.config, args.allow_timing, args.allow_physical_desktop)
        external = [Path(v) for k, v in cache.items() if k in ("Python3_EXECUTABLE", "_Python3_EXECUTABLE")]
        targets = infer_targets(selected, artifacts, build, external)
        test_argv = ctest_argv(ctest, build, args.config, selected, output, args.parallel)
        build_argv = [launcher, "--", cmake, "--build", build, "--config", args.config,
                      "--parallel", str(args.parallel), "--target", *targets]
        state.update(selectedNames=[t["name"] for t in selected], targets=targets, fileApiIndex=str(index))
        write_json(output / "plan.json", {**state, "selectedTests": selected, "buildArgv": list(map(str, build_argv)), "ctestArgv": test_argv,
            "omittedScope": ["full Python suites", "ALL_BUILD", "bundle deployment", "pluginval", "unselected CTest entries"],
            "ctestDependencies": "CTest DEPENDS orders only selected tests; omitted entries are not executed"})
        print(json.dumps({"configuration": args.config, "selectedNames": state["selectedNames"], "targets": targets}), flush=True)
        run_command(build_argv, output / "build.log", source, state)
        # Check the *exact* final selection before execution; never let a user regex broaden after planning.
        exact_inventory = json.loads(run_command(base + ["-R", exact_pattern(selected)], output / "exact-inventory.json", source, state))
        if select_tests(inventory["tests"], exact_inventory["tests"], args.config,
                        args.allow_timing, args.allow_physical_desktop) != selected:
            raise ValueError("Exact CTest selection or metadata changed after build")
        test_started = time.time_ns()
        run_command(test_argv, output / "ctest-console.log", source, state)
        state["junit"] = validate_junit(output / "ctest.xml", state["selectedNames"], test_started)
        state["sourceIdentityAfter"] = source_identity(source)
        if state["sourceIdentityAfter"] != state["sourceIdentity"]:
            raise ValueError("Source revision/dirty bytes changed during focused run")
        state["outcome"] = "FOCUSED_PASS"
        return 0
    except (ValueError, OSError, KeyError, CommandFailed, json.JSONDecodeError, subprocess.SubprocessError) as error:
        state.update(outcome="FOCUSED_FAILED", error=str(error))
        print(f"TARGETED_FAILURE {error}", file=sys.stderr, flush=True)
        return 1
    finally:
        state.update(finishedNs=time.time_ns(), elapsedSeconds=time.monotonic() - started)
        try:
            publish(state)
            print(f"{state['outcome']} {SCOPE} {state['receiptPath']}", flush=True)
        finally:
            resources.close()
            if previous_machine is None:
                os.environ.pop("AIRACK_NATIVE_REQUIRE_MACHINE", None)
            else:
                os.environ["AIRACK_NATIVE_REQUIRE_MACHINE"] = previous_machine


def main(argv=None):
    source = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=source)
    parser.add_argument("--build-root", type=Path, default=Path("build/windows-debug"))
    parser.add_argument("--test-regex", required=True)
    parser.add_argument("--config", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--native-runner", type=Path, default=source / "build/native-run/Debug/RackSynthNativeRun.exe")
    parser.add_argument("--cmake", type=Path)
    parser.add_argument("--output-root", type=Path, default=source / "build/targeted-native")
    parser.add_argument("--parallel", type=int, choices=range(1, 17), default=8)
    parser.add_argument("--allow-timing", action="store_true")
    parser.add_argument("--allow-physical-desktop", action="store_true")
    return execute(parser.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
