"""Module build check contracts, using generated-project fixtures in temporary build trees."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1] / "tools" / "juce_module_builds.py"

PROJECT = """<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ClCompile>
      <PreprocessorDefinitions>{debug}</PreprocessorDefinitions>
    </ClCompile>
    <ResourceCompile>
      <PreprocessorDefinitions>%(PreprocessorDefinitions);RESOURCE_ONLY</PreprocessorDefinitions>
    </ResourceCompile>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile>
      <PreprocessorDefinitions>{release}</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
{items}  </ItemGroup>
</Project>
"""
DEBUG_ONLY = "Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\""


class ModuleBuildTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SOURCE.is_file(), "The module build check is missing")
        spec = importlib.util.spec_from_file_location("juce_module_builds", SOURCE)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.modules = self.root / "juce" / "modules"
        self.membership = self.build / "CMakeFiles" / "TargetDirectories.txt"
        self.membership.parent.mkdir()
        self.membership.write_text("", encoding="utf-8")

    def project(self, name, sources, debug, release="NDEBUG=1;%(PreprocessorDefinitions)", items="", folder=None, active=True):
        paths = [self.modules / source if isinstance(source, str) else source for source in sources]
        items += "".join(f'    <ClCompile Include="{path}" />\n' for path in paths)
        folder = folder or self.build
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f"{name}.vcxproj").write_text(PROJECT.format(debug=debug, release=release, items=items), encoding="utf-8")
        if active:
            with self.membership.open("a", encoding="utf-8") as manifest:
                manifest.write((folder / "CMakeFiles" / f"{name}.dir").as_posix() + "\n")

    def builds(self):
        return self.module.module_builds(self.build, self.modules, "Debug")

    def run_main(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            code = self.module.main(["--build-dir", str(self.build), "--juce-modules", str(self.modules), "--config", "Debug"])
        return code, output.getvalue()

    def test_same_definitions_in_two_projects_is_a_duplicate(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], 'WIN32;JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1;RACKSYNTH_WORKSPACE="a";CMAKE_INTDIR="Debug";JUCE_SHARED_CODE=1;%(PreprocessorDefinitions)')
        self.project("Beta", [Path(self.modules.as_posix() + "/juce_core/juce_core.cpp")], 'WIN32;JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1;RACKSYNTH_WORKSPACE="b";Beta_EXPORTS;JUCE_STANDALONE_APPLICATION=1;PROBE_HOST_DIAGNOSTICS=1;%(PreprocessorDefinitions)')
        self.assertEqual(self.module.duplicate_builds(self.builds()), {"juce_core/juce_core.cpp": [["Alpha", "Beta"]]})
        code, output = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("juce_core/juce_core.cpp: 2 projects build it with the same definitions: Alpha, Beta", output)

    def test_different_definitions_are_separate_configurations(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], "WIN32;JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1;%(PreprocessorDefinitions)")
        self.project("Beta", ["juce_core/juce_core.cpp"], "WIN32;JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1;JUCE_MODAL_LOOPS_PERMITTED=1;%(PreprocessorDefinitions)")
        self.assertEqual(len(self.builds()), 2)
        self.assertEqual(self.module.duplicate_builds(self.builds()), {})
        self.assertEqual(self.run_main()[0], 0)

    def test_graphics_overlay_is_counted_as_the_same_module(self):
        overlay = self.build / "racksynth-juce" / "modules" / "juce_graphics" / "juce_graphics.cpp"
        self.project("Alpha", [overlay], "WIN32;%(PreprocessorDefinitions)")
        self.project("Beta", ["juce_graphics/juce_graphics.cpp"], "WIN32;%(PreprocessorDefinitions)")
        self.assertEqual(self.module.duplicate_builds(self.builds()), {"juce_graphics/juce_graphics.cpp": [["Alpha", "Beta"]]})
        self.assertEqual(self.run_main()[0], 1)

    def test_graphics_overlay_alone_is_not_silently_skipped(self):
        overlay = self.build / "racksynth-juce" / "modules" / "juce_graphics" / "juce_graphics.cpp"
        self.project("Alpha", [overlay], "WIN32;%(PreprocessorDefinitions)")
        self.assertEqual([build.source for build in self.builds()], ["juce_graphics/juce_graphics.cpp"])
        self.assertEqual(self.run_main()[0], 0)

    def test_overlay_does_not_reclassify_unrelated_sources(self):
        overlay = self.build / "racksynth-juce" / "modules" / "juce_core" / "juce_core.cpp"
        self.project("Alpha", [overlay], "WIN32;%(PreprocessorDefinitions)")
        self.assertEqual(self.builds(), [])

    def test_sources_outside_the_modules_are_not_module_builds(self):
        outside = [
            self.root / "src" / "plugin" / "InstrumentProcessor.cpp",
            self.root / "other" / "modules" / "juce_core" / "juce_core.cpp",
            self.root / "juce" / "modules_extra" / "juce_core" / "juce_core.cpp",
        ]
        self.project("Alpha", outside, "WIN32;%(PreprocessorDefinitions)")
        self.project("Beta", outside, "WIN32;%(PreprocessorDefinitions)")
        self.assertEqual(self.builds(), [])
        self.assertEqual(self.run_main()[0], 2)

    def test_item_definitions_and_exclusions_apply(self):
        items = (
            f'    <ClCompile Include="{self.modules / "juce_events" / "juce_events.cpp"}">\n'
            f"      <PreprocessorDefinitions {DEBUG_ONLY}>JUCE_MODAL_LOOPS_PERMITTED=1;%(PreprocessorDefinitions)</PreprocessorDefinitions>\n"
            "    </ClCompile>\n"
            f'    <ClCompile Include="{self.modules / "juce_core" / "juce_core.cpp"}">\n'
            f"      <ExcludedFromBuild {DEBUG_ONLY}>true</ExcludedFromBuild>\n"
            "    </ClCompile>\n"
        )
        self.project("Alpha", [], "WIN32;%(PreprocessorDefinitions)", items=items)
        builds = self.builds()
        self.assertEqual([build.source for build in builds], ["juce_events/juce_events.cpp"])
        self.assertEqual(builds[0].definitions, frozenset({"WIN32", "JUCE_MODAL_LOOPS_PERMITTED=1"}))

    def test_only_the_requested_configuration_counts(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], "DEBUG=1;%(PreprocessorDefinitions)", release="NDEBUG=1;%(PreprocessorDefinitions)")
        self.assertEqual(self.builds()[0].definitions, frozenset({"DEBUG=1"}))

    def test_nested_build_trees_are_not_read(self):
        self.project("Alpha", ["juce_audio_plugin_client/VST3/juce_VST3ManifestHelper.cpp"], "WIN32;%(PreprocessorDefinitions)")
        self.project("vst3_helper", ["juce_audio_plugin_client/VST3/juce_VST3ManifestHelper.cpp"], "WIN32;%(PreprocessorDefinitions)", folder=self.build / "vst3_helpers" / "Alpha")
        self.assertEqual([build.project for build in self.builds()], ["Alpha"])
        self.assertEqual(self.run_main()[0], 0)

    def test_inactive_cached_project_is_not_current(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], "WIN32;%(PreprocessorDefinitions)")
        self.project("Beta", ["juce_core/juce_core.cpp"], "WIN32;%(PreprocessorDefinitions)", active=False)
        code, output = self.run_main()
        witness = "Active metadata:\n" + self.membership.read_text(encoding="utf-8")
        for name in ("Alpha", "Beta"):
            witness += (self.build / f"{name}.vcxproj").read_text(encoding="utf-8")
        self.assertEqual(code, 0, witness + output)
        self.assertEqual([build.project for build in self.builds()], ["Alpha"])
        self.assertTrue((self.build / "Beta.vcxproj").is_file())

    def test_inactive_malformed_xml_is_not_parsed(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], "WIN32")
        self.project("Beta", ["juce_core/juce_core.cpp"], "WIN32", active=False)
        (self.build / "Beta.vcxproj").write_text("<broken", encoding="utf-8")
        self.assertEqual(self.run_main()[0], 0)
        self.assertEqual([build.project for build in self.builds()], ["Alpha"])

    def test_missing_manifest_is_an_error(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], "WIN32")
        self.membership.unlink()
        code, output = self.run_main()
        self.assertEqual(code, 2)
        self.assertIn("Cannot read current Visual Studio projects", output)

    def test_invalid_manifest_is_an_error(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], "WIN32")
        active = self.membership.read_bytes()
        cases = {
            "empty": b"",
            "blank": b"\n",
            "relative": b"CMakeFiles/Alpha.dir\n",
            "wrong_root": (self.root / "other" / "CMakeFiles" / "Alpha.dir").as_posix().encode(),
            "wrong_directory": (self.build / "other" / "Alpha.dir").as_posix().encode(),
            "duplicate_root": active + active,
            "nested_only": (self.build / "nested" / "CMakeFiles" / "Alpha.dir").as_posix().encode(),
            "missing_active": active + (self.build / "CMakeFiles" / "Missing.dir").as_posix().encode(),
            "invalid_utf8": b"\xff",
        }
        for name, contents in cases.items():
            with self.subTest(name=name):
                self.membership.write_bytes(contents)
                code, output = self.run_main()
                self.assertEqual(code, 2)
                self.assertIn("Cannot read current Visual Studio projects", output)

    def test_malformed_active_xml_is_an_error(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], "WIN32")
        (self.build / "Alpha.vcxproj").write_text("<broken", encoding="utf-8")
        code, output = self.run_main()
        self.assertEqual(code, 2)
        self.assertIn("Cannot read current Visual Studio projects", output)

    def test_appconfig_definition_remains_significant(self):
        self.project("Alpha", ["juce_core/juce_core.cpp"], 'WIN32;JUCE_APP_CONFIG_HEADER="alpha.h"')
        self.project("Beta", ["juce_core/juce_core.cpp"], 'WIN32;JUCE_APP_CONFIG_HEADER="beta.h"')
        self.assertEqual(len(self.builds()), 2)
        self.assertEqual(self.module.duplicate_builds(self.builds()), {})
        self.assertIn('JUCE_APP_CONFIG_HEADER="alpha.h"', self.builds()[0].definitions)
        self.assertEqual(self.run_main()[0], 0)

    def test_no_module_builds_is_an_error(self):
        self.assertEqual(self.run_main()[0], 2)


if __name__ == "__main__":
    unittest.main()
