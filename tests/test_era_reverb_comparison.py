import ast
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("era_comparison", ROOT / "tools/era_reverb_comparison.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class RowsTest(unittest.TestCase):
    def setUp(self):
        self.data = json.loads(tool.ROWS.read_text(encoding="utf-8"))

    def load(self, data):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "rows.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            return tool.load_rows(path)

    def test_checked_in_rows_cover_every_behaviour(self):
        rows = tool.load_rows()["rows"]
        self.assertEqual({row["behaviour"] for row in rows}, {"pre-delay", "grit", "attack", "decay", "size", "diffusion",
                         "damping", "bass", "wobble", "age", "gate", "output filters", "stereo", "mix"})
        self.assertEqual({row["metric"] for row in rows}, tool.METRICS)

    def test_rejects_unknown_fields_sources_and_repeats(self):
        for change in (lambda d: d["rows"][0]["ours"].update(width=1), lambda d: d["rows"][0].update(source="square"),
                       lambda d: d["rows"][1].update(id=d["rows"][0]["id"]), lambda d: d["rows"][0].update(seconds=2),
                       lambda d: d["rows"][0]["vintageverb"].pop("colour"), lambda d: d["neutral"].pop("mix"),
                       *(lambda d, kept=reference: d.update(rows=[r for r in d["rows"] if r["id"] != kept])
                         for reference in tool.REFERENCES.values())):
            data = copy.deepcopy(self.data)
            change(data)
            with self.assertRaises(ValueError):
                self.load(data)


class MethodsTest(unittest.TestCase):
    def test_takes_functions_and_constants_without_the_measurement_loops(self):
        module = tool.methods_source()
        functions = {node.name for node in module.body if isinstance(node, ast.FunctionDef)}
        self.assertTrue({"configure", "set_physical", "onset", "rt60", "band", "spectrum", "bandwidth_20db", "iacc",
                         "describe_ir", "mod_width", "envelope_db", "ned_profile"} <= functions)
        for node in module.body:
            self.assertIsInstance(node, (ast.Import, ast.ImportFrom, ast.FunctionDef, ast.Assign))
            if isinstance(node, ast.Assign):
                names = {n.id for t in node.targets for n in ast.walk(t) if isinstance(n, ast.Name)}
                self.assertLessEqual(names, tool.KEPT_NAMES)


@unittest.skipUnless(importlib.util.find_spec("numpy"), "the comparison measures run in the investigation venv")
class MeasureTest(unittest.TestCase):
    def setUp(self):
        import numpy as np
        self.np = np
        rng = np.random.default_rng(20261022)
        self.dry = np.zeros((2, 4800))
        self.dry[:, 100:244] = rng.standard_normal(144)
        self.kernel = rng.standard_normal(600) * np.exp(-np.arange(600) / 100)

    def convolve(self, signal, kernel):
        return self.np.array([self.np.convolve(ch, kernel)[:signal.shape[1]] for ch in signal])

    def test_deconvolve_recovers_the_impulse_response_behind_a_burst(self):
        response = self.np.zeros(600)
        response[[0, 37, 401]] = (1, .5, -.25)
        recovered = tool.deconvolve(self.convolve(self.dry, response), self.dry, 1e-12)
        self.np.testing.assert_allclose(recovered[:, :600], self.np.tile(response, (2, 1)), atol=1e-6)

    def test_dry_gain_removes_a_wet_that_starts_with_the_dry(self):
        wet = self.convolve(self.dry, self.kernel)
        processed = self.np.cos(self.np.pi / 4) * self.dry + self.np.sin(self.np.pi / 4) * wet
        methods = {"RATE": 48000, "onset": lambda signal: 100}
        got = tool.measure(methods, "dry_gain", processed, self.dry, wet)
        self.assertAlmostEqual(got["dry_gain_wet_removed"], self.np.cos(self.np.pi / 4), places=9)
        self.assertAlmostEqual(got["wet_gain"], self.np.sin(self.np.pi / 4), places=9)
        self.assertGreater(abs(got["dry_gain"] - self.np.cos(self.np.pi / 4)), .01)

    def test_diffuse_time_skips_windows_sixty_db_under_the_peak(self):
        np = self.np
        rng = np.random.default_rng(7)
        start = {"RATE": 48000, "onset": lambda signal: 0}
        sparse = 1e-5 * rng.standard_normal((2, 48000))
        sparse[:, ::2400] = 1
        self.assertIsNone(tool.diffuse_ms(start, sparse))
        dense = rng.standard_normal((2, 48000)) * np.exp(-np.arange(48000) / 9600)
        self.assertLessEqual(tool.diffuse_ms(start, dense), 15)

    def test_swell_peak_counts_from_the_input_too(self):
        np = self.np
        processed = np.zeros((2, 48000))
        processed[:, 4900] = 1e-3
        processed[:, 12000:12480] = .5
        loud = lambda signal: int(np.argmax(np.max(np.abs(signal), axis=0) > .01 * np.max(np.abs(signal))))
        power = lambda signal, ms: 10 * np.log10(np.convolve(np.sum(signal ** 2, axis=0), np.ones(480) / 480, "same") + 1e-24)
        got = tool.measure({"RATE": 48000, "onset": loud, "envelope_db": power}, "attack_ms", processed, self.dry)
        self.assertAlmostEqual(got["attack_from_input_ms"], (12240 - 100) / 48, delta=1)
        self.assertAlmostEqual(got["attack_ms"], (12240 - 12000) / 48, delta=1)

    def test_early_and_late_windows_start_at_the_dry_onset(self):
        np = self.np
        processed = np.zeros((2, 48000))
        processed[:, 110] = 1e-3
        processed[:, 5760:9600] = .5 * np.sin(np.arange(3840) * .3)
        loud = lambda signal: int(np.argmax(np.max(np.abs(signal), axis=0) > .01 * np.max(np.abs(signal))))
        power = lambda signal, ms: 10 * np.log10(np.convolve(np.sum(signal ** 2, axis=0), np.ones(480) / 480, "same") + 1e-24)
        got = tool.measure({"RATE": 48000, "onset": loud, "envelope_db": power}, "early_late_db", processed, self.dry)
        self.assertGreater(got["late_minus_early_db"], 40)
        self.assertTrue(120 <= got["tail_peak_ms"] <= 200)


if __name__ == "__main__":
    unittest.main()
