import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

research_dependencies_available = all(
    importlib.util.find_spec(name) is not None for name in ("numpy", "soundfile")
)
if research_dependencies_available:
    import numpy as np
    import soundfile as sf

    path = Path(__file__).parents[1] / "tools/drum_generation_trial.py"
    spec = importlib.util.spec_from_file_location("drum_trial", path)
    trial = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(trial)

@unittest.skipUnless(research_dependencies_available,
                     "Optional YuE2 research dependencies NumPy and SoundFile require the pinned ComfyUI interpreter")
class TrialTests(unittest.TestCase):
    def test_fixed_matrix_and_direct_graph(self):
        cases = trial.matrix()
        self.assertEqual(len(cases), 12)
        self.assertEqual({(k, s) for k, s, _ in cases},
                         {(k, s) for k in ("kick", "snare", "hat", "percussion")
                          for s in (17, 73, 211)})
        for kind, seed, style in cases:
            graph = trial.graph(kind, seed, style)
            self.assertEqual(graph["2"]["inputs"]["lyrics"], "")
            self.assertEqual(graph["2"]["inputs"]["abc"], "")
            self.assertEqual(graph["4"]["inputs"]["seconds"], ["2", 1])
            self.assertEqual(graph["5"]["inputs"]["seed"], seed)
            self.assertEqual(graph["5"]["inputs"]["sampler_name"], "dpm_2")

    def test_prompts_and_all_graph_settings_are_frozen(self):
        prompts = {
            "kick": "A single dry electronic kick drum hit, deep and punchy",
            "snare": "A single dry acoustic snare drum hit, crisp and punchy",
            "hat": "A single dry closed hi-hat hit, short and crisp",
            "percussion": "A single dry hand-played conga hit, warm and percussive",
        }
        tail = (", with a complete natural decay and silence before and after. "
                "Isolated sound, no other instruments, rhythm, music, vocals or effects.")
        baseline = None
        for kind, seed, style in trial.matrix():
            self.assertEqual(style, prompts[kind] + tail)
            graph = trial.graph(kind, seed, style)
            self.assertEqual(set(graph), set("1234567"))
            self.assertEqual(graph["1"], {"class_type": "CheckpointLoaderSimple",
                                         "inputs": {"ckpt_name": "yue2_3b_bf16.safetensors"}})
            self.assertEqual(graph["2"]["inputs"], {
                "clip": ["1", 1], "style": style, "lyrics": "", "abc": "",
                "mode": "full", "seed": seed, "max_duration": 10, "temperature": 1,
                "top_p": 0.95, "top_k": 100, "repetition_penalty": 1.2, "cfg_scale": 1.01,
            })
            self.assertEqual(graph["5"]["inputs"], {
                "model": ["1", 0], "positive": ["2", 0], "negative": ["3", 0],
                "latent_image": ["4", 0], "seed": seed, "steps": 32, "cfg": 1,
                "sampler_name": "dpm_2", "scheduler": "sgm_uniform", "denoise": 1,
            })
            self.assertEqual(graph["7"]["inputs"]["filename_prefix"], f"trial/{kind}-{seed}")
            self.assertEqual(graph["7"]["inputs"]["format"], "flac")
            graph["2"]["inputs"].update(style="", seed=0)
            graph["5"]["inputs"]["seed"] = 0
            graph["7"]["inputs"]["filename_prefix"] = ""
            if baseline is None:
                baseline = graph
            self.assertEqual(graph, baseline)

    def test_crop_changes_only_selection_and_gain(self):
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / "in.wav", Path(directory) / "out.wav"
            audio = np.arange(40, dtype=np.float32).reshape(20, 2) / 100
            sf.write(source, audio, 48000, subtype="FLOAT")
            trial.prepare(source, target, 3, 17, 0.5)
            actual, rate = sf.read(target, dtype="float32", always_2d=True)
            self.assertEqual(rate, 48000)
            np.testing.assert_array_equal(actual, audio[3:17] * np.float32(0.5))
            with self.assertRaises(FileExistsError):
                trial.prepare(source, target, 3, 17, 0.5)
            with self.assertRaises(ValueError):
                trial.prepare(source, Path(directory) / "bad.wav", 9, 8, 1)

    def test_prepare_rejects_bad_signal_and_geometry_before_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for label, audio, rate, first, last, gain in (
                ("clipped", np.full((20, 2), 0.5), 48000, 0, 20, 2),
                ("nonfinite", np.full((20, 1), np.nan), 48000, 0, 20, 1),
                ("channels", np.zeros((20, 3)), 48000, 0, 20, 1),
                ("rate", np.zeros((20, 1)), 4000, 0, 20, 1),
                ("duration", np.zeros((80001, 1)), 8000, 0, 80001, 1),
                ("gain", np.zeros((20, 1)), 48000, 0, 20, float("nan")),
                ("fractional_frame", np.zeros((20, 1)), 48000, 0.5, 20, 1),
            ):
                with self.subTest(label=label):
                    source, target = root / f"{label}.wav", root / f"{label}-out.wav"
                    sf.write(source, audio, rate, subtype="FLOAT")
                    with self.assertRaises(ValueError):
                        trial.prepare(source, target, first, last, gain)
                    self.assertFalse(target.exists())

    def test_success_keeps_all_attempts_and_never_scores_quality_or_timing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "base/output/trial/raw.flac"
            output.parent.mkdir(parents=True)
            sf.write(output, np.zeros((48, 1)), 48000)
            history = {"status": {"status_str": "success"}, "outputs": {"7": {"audio": [
                {"type": "output", "subfolder": "trial", "filename": "raw.flac"}
            ]}}}
            with patch.object(trial, "gpu", return_value={"exit": 0}), patch.object(
                trial, "request", side_effect=[{"prompt_id": "id"}, {"id": history}] * 3
            ) as request:
                result = trial.run(root, "kick")
            self.assertEqual(request.call_count, 6)
            self.assertEqual([a["seed"] for a in result["attempts"]], [17, 73, 211])
            self.assertTrue(all(a["status"] == "generated" for a in result["attempts"]))
            self.assertTrue(all(a["truncated"] == "unknown" for a in result["attempts"]))
            self.assertTrue(all(a["listening"] == "pending" for a in result["attempts"]))
            self.assertTrue(all(a["distinct_variant"] is None for a in result["attempts"]))
            self.assertEqual(result["quality"], "pending_user_listening")
            self.assertFalse(result["timing_qualified"])
            self.assertEqual(result["unrun_seeds"], [])
            with patch.object(trial, "gpu") as gpu, patch.object(trial, "request") as request:
                with self.assertRaises(FileExistsError):
                    trial.run(root, "kick")
                gpu.assert_not_called()
                request.assert_not_called()

    def test_server_failure_preserves_history_and_does_not_reroll(self):
        history = {"status": {"status_str": "error", "messages": [
            ["execution_error", {"exception_message": "original failure"}]
        ]}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(trial, "gpu", return_value={"exit": 0}), patch.object(
                trial, "request", side_effect=[{"prompt_id": "id"}, {"id": history}]
            ) as request:
                result = trial.run(root, "snare")
            self.assertEqual(request.call_count, 2)
            self.assertEqual(result["unrun_seeds"], [73, 211])
            self.assertEqual(result["attempts"][0]["status"], "error")
            self.assertTrue(result["attempts"][0]["requires_owned_server_shutdown"])
            self.assertEqual(json.loads((root / "attempts/snare/17-history.json").read_text()), history)

    def test_timeout_stops_submission_and_preserves_unrun_seeds(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(trial, "gpu", return_value={"exit": 0}), patch.object(
                trial, "request", side_effect=[{"prompt_id": "id"}, {}]
            ) as request, patch.object(trial.time, "monotonic", side_effect=[0, 301]):
                result = trial.run(directory, "hat")
            self.assertEqual(request.call_count, 2)
            self.assertIn("TimeoutError", result["attempts"][0]["error"])
            self.assertTrue(result["attempts"][0]["requires_owned_server_shutdown"])
            self.assertEqual(result["unrun_seeds"], [73, 211])
            self.assertTrue((Path(directory) / "attempts/hat/summary.json").is_file())

    def test_http_error_body_is_retained(self):
        error = urllib.error.HTTPError("http://127.0.0.1:8189/prompt", 400,
                                       "Bad Request", {}, io.BytesIO(b'{"error":"bad node"}'))
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(trial, "gpu", return_value={"exit": 0}), patch.object(
                trial, "request", side_effect=error
            ) as request:
                result = trial.run(directory, "percussion")
            self.assertEqual(request.call_count, 1)
            attempt = result["attempts"][0]
            self.assertEqual(attempt["http_status"], 400)
            self.assertEqual(attempt["http_body"], '{"error":"bad node"}')

    def test_http_error_body_is_bounded_and_read_errors_keep_original_failure(self):
        for body in (io.BytesIO(b"x" * 65537), unittest.mock.Mock()):
            if isinstance(body, unittest.mock.Mock):
                body.read.side_effect = OSError("body unavailable")
            error = urllib.error.HTTPError("http://127.0.0.1:8189/prompt", 500,
                                           "Original failure", {}, body)
            with self.subTest(body=type(body).__name__), tempfile.TemporaryDirectory() as directory:
                with patch.object(trial, "gpu", return_value={"exit": 0}), patch.object(
                    trial, "request", side_effect=error
                ):
                    result = trial.run(directory, "percussion")
                attempt = result["attempts"][0]
                self.assertIn("Original failure", attempt["error"])
                self.assertEqual(attempt["http_status"], 500)
                if isinstance(body, io.BytesIO):
                    self.assertEqual(len(attempt["http_body"]), 65536)
                    self.assertTrue(attempt["http_body_truncated"])
                else:
                    self.assertIn("body unavailable", attempt["http_body_error"])
                self.assertEqual(result["unrun_seeds"], [73, 211])

    def test_escaped_output_is_rejected_before_audio_read(self):
        history = {"status": {"status_str": "success"}, "outputs": {"7": {"audio": [
            {"type": "output", "subfolder": "../../../", "filename": "foreign.wav"}
        ]}}}
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(trial, "gpu", return_value={"exit": 0}), patch.object(
                trial, "request", side_effect=[{"prompt_id": "id"}, {"id": history}]
            ), patch.object(trial.sf, "info") as info:
                result = trial.run(directory, "kick")
            info.assert_not_called()
            self.assertIn("escaped", result["attempts"][0]["error"])

    def test_warmup_is_separate_and_has_only_seed_1009(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(trial, "gpu", return_value={"exit": 0}), patch.object(
                trial, "request", side_effect=ConnectionError("no server")
            ) as request:
                result = trial.run(directory, "kick", warmup=True)
            self.assertEqual(request.call_count, 1)
            self.assertEqual([a["seed"] for a in result["attempts"]], [1009])
            self.assertEqual(result["quality"], "not_scored")
            prompt = json.loads((Path(directory) / "smoke/kick/1009-prompt.json").read_text())
            self.assertEqual(prompt["7"]["inputs"]["filename_prefix"], "trial/warmup-kick-1009")
            self.assertFalse((Path(directory) / "attempts").exists())

    def test_missing_telemetry_is_an_explicit_observation(self):
        with patch.object(trial.subprocess, "run", side_effect=FileNotFoundError("nvidia-smi")):
            observation = trial.gpu()
        self.assertIsNone(observation["exit"])
        self.assertIn("FileNotFoundError", observation["error"])


if __name__ == "__main__":
    unittest.main()
