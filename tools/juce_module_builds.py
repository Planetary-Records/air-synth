"""Fail when two Visual Studio projects compile one JUCE module source with the same definitions; development tooling only."""
import argparse
import os
from pathlib import Path
import re
import sys
from typing import NamedTuple
import xml.etree.ElementTree as ElementTree

MSBUILD = "{http://schemas.microsoft.com/developer/msbuild/2003}"
INHERITED = "%(PreprocessorDefinitions)"
# Definitions that cannot change a module's code at the pinned JUCE. No JUCE source reads CMake's CMAKE_INTDIR and
# <target>_EXPORTS or the project's RACKSYNTH_*. JUCE's CMake sets JUCE_SHARED_CODE on a plugin's shared code and
# JUCE_STANDALONE_APPLICATION on console apps, and no module reads either. The probe's patch reads
# PROBE_HOST_DIAGNOSTICS only in the VST3 client source, which only the probe's wrapper builds.
IGNORED = re.compile(r"(CMAKE_INTDIR|JUCE_SHARED_CODE|JUCE_STANDALONE_APPLICATION|PROBE_HOST_DIAGNOSTICS|RACKSYNTH_\w*|\w+_EXPORTS)(=.*)?")


class ModuleBuild(NamedTuple):
    project: str
    source: str
    definitions: frozenset[str]


def _for_config(element, config: str) -> bool:
    return f"'{config}|" in element.get("Condition", f"'{config}|")


def _definitions(text: str | None, inherited: list[str]) -> list[str]:
    result = []
    for definition in (text or "").split(";"):
        definition = definition.strip()
        if definition == INHERITED:
            result += inherited
        elif definition and not definition.startswith("%("):
            result.append(definition)
    return result


def _module_source(include: str, project_dir: Path, modules: str) -> str | None:
    path = os.path.abspath(project_dir / include)
    if not os.path.normcase(path).startswith(modules):
        return None
    return path[len(modules):].replace("\\", "/")


def _current_projects(build_dir: Path) -> list[Path]:
    build_dir = build_dir.resolve()
    manifest = build_dir / "CMakeFiles" / "TargetDirectories.txt"
    projects = set()
    for line in manifest.read_text(encoding="utf-8-sig").splitlines():
        directory = Path(line)
        if not line or line != line.strip() or not directory.is_absolute() or directory.suffix != ".dir":
            raise ValueError(f"Malformed target directory in {manifest}: {line!r}")
        directory = directory.resolve()
        if not directory.is_relative_to(build_dir) or directory.parent.name != "CMakeFiles":
            raise ValueError(f"Target directory is outside this build's CMakeFiles: {line!r}")
        if directory.parent != build_dir / "CMakeFiles":
            continue
        project = build_dir / f"{directory.stem}.vcxproj"
        if project in projects or not project.is_file():
            raise ValueError(f"Duplicate or missing current project: {project}")
        projects.add(project)
    if not projects:
        raise ValueError(f"No current top-level projects in {manifest}")
    return sorted(projects)


def module_builds(build_dir: Path, juce_modules: Path, config: str) -> list[ModuleBuild]:
    modules = os.path.normcase(os.path.abspath(juce_modules)).rstrip("\\/") + os.sep
    graphics = os.path.normcase(os.path.abspath(build_dir / "racksynth-juce" / "modules")).rstrip("\\/") + os.sep
    builds = []
    # Current top-level targets only; cached projects and nested JUCE helper builds are not this graph.
    for project in _current_projects(Path(build_dir)):
        root = ElementTree.parse(project).getroot()
        defaults = []
        for group in root.iter(f"{MSBUILD}ItemDefinitionGroup"):
            if _for_config(group, config):
                for element in group.iterfind(f"{MSBUILD}ClCompile/{MSBUILD}PreprocessorDefinitions"):
                    defaults = _definitions(element.text, [])
        for item in root.iter(f"{MSBUILD}ClCompile"):
            source = _module_source(item.get("Include", ""), project.parent, modules) if item.get("Include") else None
            if source is None and item.get("Include"):
                overlay = _module_source(item.get("Include"), project.parent, graphics)
                if overlay is not None and overlay.startswith("juce_graphics/"):
                    source = overlay
            if source is None:
                continue
            excluded = [e for e in item.iterfind(f"{MSBUILD}ExcludedFromBuild") if _for_config(e, config)]
            if any((e.text or "").strip().lower() == "true" for e in excluded):
                continue
            definitions = defaults
            for element in item.iterfind(f"{MSBUILD}PreprocessorDefinitions"):
                if _for_config(element, config):
                    definitions = _definitions(element.text, defaults)
            kept = frozenset(d for d in definitions if not IGNORED.fullmatch(d))
            builds.append(ModuleBuild(project.stem, source, kept))
    return sorted(builds, key=lambda build: (build.project, build.source))


def duplicate_builds(builds: list[ModuleBuild]) -> dict[str, list[list[str]]]:
    groups: dict[str, dict[frozenset[str], list[str]]] = {}
    for build in builds:
        groups.setdefault(build.source, {}).setdefault(build.definitions, []).append(build.project)
    duplicates = {}
    for source in sorted(groups):
        repeated = sorted(sorted(projects) for projects in groups[source].values() if len(projects) > 1)
        if repeated:
            duplicates[source] = repeated
    return duplicates


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--juce-modules", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    args = parser.parse_args(argv)
    try:
        builds = module_builds(args.build_dir, args.juce_modules, args.config)
    except (OSError, ValueError, ElementTree.ParseError) as error:
        print(f"Cannot read current Visual Studio projects: {error}", file=sys.stderr)
        return 2
    if not builds:
        print(f"No project in {args.build_dir} compiles a source from {args.juce_modules} in {args.config}.")
        return 2
    projects = sorted({build.project for build in builds})
    print(f"{len(builds)} JUCE module builds of {len({build.source for build in builds})} sources in {len(projects)} projects, "
          f"under {len({build.definitions for build in builds})} sets of definitions")
    for project in projects:
        print(f"  {project}: {sum(build.project == project for build in builds)}")
    duplicates = duplicate_builds(builds)
    for source, groups in duplicates.items():
        for group in groups:
            print(f"{source}: {len(group)} projects build it with the same definitions: {', '.join(group)}")
    if duplicates:
        return 1
    print("Each JUCE module source is built once per set of definitions.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
