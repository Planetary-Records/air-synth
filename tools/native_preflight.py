"""Read-only Windows inventory for M0. Presence is never execution proof.

Exit 0: inventory completed (dependencies may be missing).
Exit 1: discovery failed or this OS is unsupported. Exit 2: invalid CLI/output.
Only --output writes a file; no installers, compiler or host are executed.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def _record(paths=()):
    paths = sorted({str(p) for p in paths if Path(p).is_file()})
    return {"status": "found" if paths else "missing", "paths": paths}


def _roots(*names):
    return list(dict.fromkeys(Path(os.environ[n]) for n in names if os.environ.get(n)))


def _read_ableton_uninstall():
    """Read only application identity fields, never uninstall commands or tokens."""
    import winreg

    records = []
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
            try:
                key = winreg.OpenKey(hive, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall", 0, winreg.KEY_READ | view)
            except FileNotFoundError:
                continue
            with key:
                for index in range(winreg.QueryInfoKey(key)[0]):
                    try:
                        subkey = winreg.OpenKey(key, winreg.EnumKey(key, index), 0, winreg.KEY_READ | view)
                    except FileNotFoundError:
                        continue
                    with subkey:
                        record = {}
                        for name in ("DisplayName", "Publisher", "DisplayVersion", "InstallLocation"):
                            try:
                                value, _ = winreg.QueryValueEx(subkey, name)
                            except FileNotFoundError:
                                continue
                            if isinstance(value, str):
                                record[name] = value
                        if re.match(r"^Ableton Live \d+\b", record.get("DisplayName", ""), re.I):
                            records.append(record)
    return records


def collect_environment() -> dict:
    report = {
        "schema_version": 1,
        "platform": {"system": "Windows" if sys.platform == "win32" else sys.platform},
        "tools": {name: _record() for name in ("cmake", "vswhere", "msvc", "windows_sdk")},
        "ableton": {**_record(), "installations": []},
        "gates": {"native_probe_ready": False, "native_build_verified": False, "ableton_host_verified": False},
        "diagnostics": [],
    }

    def diagnostic(code, message, severity="warning"):
        report["diagnostics"].append({"code": code, "severity": severity, "message": message})

    if report["platform"]["system"] != "Windows":
        diagnostic("unsupported_platform", "This preflight supports Windows only.", "error")
        return report
    report["platform"]["windows_version"] = list(sys.getwindowsversion()[:3])

    vs_discovery_ready = True
    try:
        program_roots = _roots("ProgramFiles", "ProgramFiles(x86)")
        vs_paths = [p for root in program_roots for p in (root / "Microsoft Visual Studio/2022").glob("*") if p.is_dir()]
        vswhere_paths = [root / "Microsoft Visual Studio/Installer/vswhere.exe" for root in program_roots]
        for name in ("cmake", "vswhere", "cl"):
            path = shutil.which(name)
            if path:
                if name == "cl":
                    # A PATH entry alone is not a complete MSVC toolchain.
                    report["tools"]["msvc"]["path_compiler"] = str(path)
                    compiler = Path(path)
                    if compiler.is_file():
                        for parent in compiler.parents:
                            if parent.name == "MSVC" and parent.parent.name == "Tools" and parent.parent.parent.name == "VC":
                                vs_paths.append(parent.parent.parent.parent)
                elif name == "vswhere":
                    vswhere_paths.insert(0, Path(path))
                else:
                    report["tools"]["cmake"] = _record([path])
        report["tools"]["vswhere"] = _record(vswhere_paths)
        rejected_vs = set()
        if report["tools"]["vswhere"]["paths"]:
            executable = report["tools"]["vswhere"]["paths"][0]
            try:
                result = subprocess.run(
                    [executable, "-products", "*", "-version", "[17.0,18.0)", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json", "-utf8"],
                    capture_output=True, text=True, encoding="utf-8", timeout=10, check=False,
                )
                if result.returncode:
                    diagnostic("vswhere_failed", f"vswhere exited {result.returncode}; inspect the Visual Studio installation.", "error")
                else:
                    instances = json.loads(result.stdout)
                    if not isinstance(instances, list) or any(not isinstance(item, dict) for item in instances):
                        raise ValueError("Expected instance array")
                    if not instances:
                        vs_discovery_ready = False
                        diagnostic("vswhere_empty", "vswhere found no VS 2022 instance with the C++ component.")
                    for instance in instances:
                        path = instance.get("installationPath")
                        valid = isinstance(path, str) and Path(path).is_absolute()
                        valid = valid and instance.get("isComplete") is True and instance.get("isLaunchable") is True
                        valid = valid and str(instance.get("installationVersion", "")).startswith("17.")
                        if valid:
                            vs_paths.append(Path(path))
                        else:
                            vs_discovery_ready = False
                            if isinstance(path, str):
                                rejected_vs.add(Path(path))
                            diagnostic("vswhere_incomplete", "An incomplete or invalid VS instance was ignored.")
            except (OSError, subprocess.SubprocessError, ValueError, UnicodeError) as error:
                diagnostic("vswhere_error", f"Cannot read VS discovery ({type(error).__name__}); repair or retry vswhere.", "error")
        else:
            diagnostic("vswhere_missing", "vswhere was not found; inspecting standard VS 2022 locations.")

        compiler_paths = []
        cmake_paths = list(report["tools"]["cmake"]["paths"])
        for install in sorted(set(vs_paths) - rejected_vs):
            cmake_paths.append(install / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
            for toolset in (install / "VC/Tools/MSVC").glob("*"):
                required = [toolset / name for name in ("bin/Hostx64/x64/cl.exe", "bin/Hostx64/x64/link.exe", "include/vector", "lib/x64/libcmt.lib")]
                if all(p.is_file() for p in required):
                    compiler_paths.append(required[0])
        report["tools"]["cmake"] = _record(cmake_paths)
        report["tools"]["msvc"].update(_record(compiler_paths))

        sdk_paths = []
        sdk_roots = [root / "Windows Kits/10" for root in program_roots]
        report["tools"]["windows_sdk"]["searched_roots"] = [str(p) for p in sdk_roots]
        for sdk in sdk_roots:
            for include in (sdk / "Include").glob("10.*"):
                required = [
                    include / "um/Windows.h", include / "shared/sdkddkver.h", include / "ucrt/stdio.h",
                    sdk / "Lib" / include.name / "um/x64/kernel32.lib",
                    sdk / "Lib" / include.name / "ucrt/x64/ucrt.lib",
                    sdk / "bin" / include.name / "x64/rc.exe",
                    sdk / "bin" / include.name / "x64/mt.exe",
                ]
                if all(p.is_file() for p in required):
                    sdk_paths.append(required[0])
        report["tools"]["windows_sdk"].update(_record(sdk_paths))

        installations = {}
        candidates = []
        for record in _read_ableton_uninstall():
            if re.match(r"^Ableton Live \d+\b", record.get("DisplayName", ""), re.I) and "ableton" in record.get("Publisher", "").lower():
                location = record.get("InstallLocation", "").strip().strip('"')
                if location and Path(location).is_absolute():
                    candidates.append((Path(location), record.get("DisplayVersion"), "uninstall_metadata"))
        for root in _roots("ProgramData", "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"):
            for location in (root / "Ableton").glob("Live *"):
                candidates.append((location, None, "known_install_location"))
        for location, version, source in candidates:
            for executable in (location / "Program").glob("Ableton Live *.exe"):
                if executable.is_file() and re.fullmatch(r"Ableton Live \d+(?:[ .][\w .-]+)?\.exe", executable.name, re.I):
                    installations.setdefault(str(executable), {"path": str(executable), "version": version, "source": source})
        report["ableton"] = {**_record(installations), "installations": list(installations.values())}
    except (OSError, ValueError) as error:
        diagnostic("discovery_error", f"Discovery could not complete ({type(error).__name__}); check access to installation files and uninstall metadata.", "error")

    for name in ("cmake", "msvc", "windows_sdk"):
        if report["tools"][name]["status"] == "missing":
            diagnostic(name + "_missing", f"No complete {name} installation found; install or locate the M0 prerequisite.")
    if report["ableton"]["status"] == "missing":
        diagnostic("ableton_missing", "Installed Ableton Live was not found in uninstall metadata or standard locations; a download is not an installation.")
    report["gates"]["native_probe_ready"] = (
        all(report["tools"][name]["status"] == "found" for name in ("cmake", "msvc", "windows_sdk"))
        and vs_discovery_ready
        and not any(d["severity"] == "error" for d in report["diagnostics"])
    )
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    report = collect_environment()
    try:
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"output_error: cannot write report ({type(error).__name__}); use a writable file in an existing directory.", file=sys.stderr)
        return 2
    return 1 if any(d["severity"] == "error" for d in report["diagnostics"]) else 0


if __name__ == "__main__":
    sys.exit(main())
