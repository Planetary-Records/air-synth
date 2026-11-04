"""Routed group check contracts, on synthetic test mains and the real routed test source."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools" / "routed_groups.py"
ROUTED = ROOT / "tests" / "native" / "RoutedSynthEngineTests.cpp"

MAIN = """
void one() {}
int main()
{
    if(const auto* group=std::getenv("AIRACK_ROUTED_TEST_GROUP")){
        const std::string_view name(group);
        for(const char* flag : {"X_ONLY"}) if(std::getenv(flag)){std::cout<<"conflict="<<flag<<'\\n';return 2;}
        if(name=="a"){
            one();if(failures){std::cout<<"checks="<<checks<<'\\n';return 1;}
            two<Engine>();
        }
        %s
        else{std::cout<<"invalid="<<name<<'\\n';return 2;}
        return failures?1:0;
    }
    one();two<Engine>();if(std::getenv("X_ONLY")){return 0;}three();
    return failures?1:0;
}
"""


class RoutedGroupTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SOURCE.is_file(), "The routed group check is missing")
        spec = importlib.util.spec_from_file_location("routed_groups", SOURCE)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def problems(self, other_groups, names):
        groups, default = self.module.parse(MAIN % other_groups)
        return self.module.check(groups, default, names)

    def test_groups_holding_the_default_run_once_pass(self):
        groups, default = self.module.parse(MAIN % 'else if(name=="b"){three();}')
        self.assertEqual({"a": {"one", "two"}, "b": {"three"}}, groups)
        self.assertEqual({"one", "two", "three"}, default)
        self.assertEqual([], self.module.check(groups, default, ["a", "b"]))

    def test_a_branch_only_a_set_flag_takes_is_not_the_default_run(self):
        source = MAIN.replace("one();two<Engine>();", 'one();two<Engine>();if(std::getenv("Y_ONLY")){probe();return 0;}'
                              'if(!std::getenv("Z_ONLY")){four();}')
        groups, default = self.module.parse(source % 'else if(name=="b"){three();four();}')
        self.assertEqual({"one", "two", "three", "four"}, default)
        self.assertEqual([], self.module.check(groups, default, ["a", "b"]))

    def test_a_default_call_no_named_group_holds_is_missing(self):
        self.assertEqual(["missing: three"], self.problems('else if(name=="b"){three();}', ["a"]))

    def test_a_call_two_named_groups_hold_runs_twice(self):
        self.assertEqual(["twice: one in a, b"], self.problems('else if(name=="b"){one();three();}', ["a", "b"]))

    def test_a_name_without_a_group_is_unknown(self):
        self.assertEqual(["unknown group: c"], self.problems('else if(name=="b"){three();}', ["a", "b", "c"]))

    def test_no_names_is_a_problem(self):
        self.assertIn("no groups named", self.problems('else if(name=="b"){three();}', []))

    def test_the_command_reports_and_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "main.cpp"
            source.write_text(MAIN % 'else if(name=="b"){three();}', encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                failed = self.module.main(["--source", str(source), "a"])
                passed = self.module.main(["--source", str(source), "a", "b"])
        self.assertEqual((1, 0), (failed, passed))
        self.assertIn("missing: three", output.getvalue())

    def test_the_routed_source_parses(self):
        groups, default = self.module.parse(ROUTED.read_text(encoding="utf-8"))
        for name in ("native_prefix_head", "native_prefix_tail", "shared_dimension", "stereo", "flanger", "phaser",
                     "chorus_eq_filter", "legacy_core", "modern_reverb", "modern_drive", "modern_compressor",
                     "modern_modulated_taps", "modern_delay", "modern_eq", "modern_filter"):
            self.assertTrue(groups.get(name), name)
        self.assertIn("baseFrameMathLiveArguments", default)
        self.assertIn("preparedQualityContract", default)


if __name__ == "__main__":
    unittest.main()
