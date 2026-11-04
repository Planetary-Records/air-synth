import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


research_dependencies_available = all(
    importlib.util.find_spec(name) is not None for name in ("numpy", "soundfile")
)
stable = None
if research_dependencies_available:
    import numpy as np
    import soundfile as sf

    tools = Path(__file__).parents[1] / "tools"
    path = tools / "stable_audio3_drum_trial.py"
    if path.exists():
        spec = importlib.util.spec_from_file_location("stable_trial", path)
        stable = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(stable)


@unittest.skipUnless(research_dependencies_available,
                     "Optional Stable Audio research dependencies require the pinned ComfyUI interpreter")
class StableAudioTrialTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(stable, "Stable Audio 3 trial wrapper is missing")

    def histories(self, root, *, rate=44100, audio=None):
        output = root / "base/output/trial"
        output.mkdir(parents=True)
        replies = []
        for seed in (17, 73, 211):
            source = output / f"{seed}.wav"
            samples = (np.arange(40, dtype=np.float32).reshape(20, 2) / 100
                       if audio is None else audio)
            sf.write(source, samples, rate, subtype="FLOAT")
            history = {"status": {"status_str": "success"}, "outputs": {"7": {"audio": [
                {"type": "output", "subfolder": "trial", "filename": source.name}
            ]}}}
            replies.extend([{"prompt_id": str(seed)}, {str(seed): history}])
        return replies

    def run_trial(self, root, replies, kind="kick", warmup=False):
        with patch.object(stable.trial, "gpu", return_value={"exit": 0}), patch.object(
            stable.trial, "request", side_effect=replies
        ):
            return stable.run(root, kind, warmup)

    def test_cli_help_runs_in_the_same_embedded_interpreter(self):
        with tempfile.TemporaryDirectory() as directory:
            completed = subprocess.run([sys.executable, "-B", str(path), "--help"],
                                       cwd=directory, capture_output=True, text=True, timeout=30)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("--warmup", completed.stdout)
        self.assertIn("kick,snare,hat,percussion", completed.stdout)

    def test_frozen_matrix_and_complete_graph(self):
        self.assertEqual(len(stable.trial.matrix()), 12)
        for kind, seed, style in stable.trial.matrix():
            self.assertIn(seed, (17, 73, 211))
            self.assertEqual(style, stable.trial.DESCRIPTIONS[kind] + stable.trial.TAIL)
            def node(name, **inputs):
                return {"class_type": name, "inputs": inputs}
            self.assertEqual(stable.graph(kind, seed, style), {
                "1": node("CheckpointLoaderSimple", ckpt_name="stable_audio_3_small_sfx.safetensors"),
                "2": node("CLIPLoader", clip_name="t5gemma_b_b_ul2.safetensors", type="stable_audio", device="default"),
                "3": node("CLIPTextEncode", clip=["2", 0], text=style),
                "8": node("CLIPTextEncode", clip=["2", 0], text=""),
                "4": node("EmptyLatentAudio", seconds=10, batch_size=1),
                "5": node("KSampler", model=["1", 0], positive=["3", 0], negative=["8", 0],
                          latent_image=["4", 0], seed=seed, steps=8, cfg=1, sampler_name="lcm",
                          scheduler="simple", denoise=1),
                "6": node("VAEDecodeAudio", samples=["5", 0], vae=["1", 2]),
                "7": node("SaveAudioAdvanced", audio=["6", 0], filename_prefix=f"trial/{kind}-{seed}", format="flac"),
            })

    def test_success_copies_three_originals_exactly_and_preserves_immutable_records(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.run_trial(root, self.histories(root))
            self.assertEqual(result["unrun_seeds"], [])
            receipt_path = root / "attempts/kick/audition.json"
            receipt = json.loads(receipt_path.read_text())
            self.assertEqual(result["audition"], receipt)
            self.assertEqual(receipt["quality"], "pending_user_listening")
            self.assertFalse(receipt["timing_qualified"])
            self.assertGreaterEqual(receipt["seconds"], receipt["generation_seconds"])
            self.assertGreaterEqual(receipt["copy_seconds"], 0)
            self.assertEqual(receipt["uncopied_seeds"], [])
            self.assertEqual([copy["seed"] for copy in receipt["copies"]], [17, 73, 211])
            shared_summary = json.loads((root / "attempts/kick/summary.json").read_text())
            self.assertNotIn("audition", shared_summary)
            for record, copy in zip(result["attempts"], receipt["copies"]):
                self.assertEqual(record["status"], "generated")
                self.assertEqual(copy["status"], "copied")
                self.assertEqual(record, json.loads((root / f"attempts/kick/{record['seed']}-result.json").read_text()))
                target = root / f"attempts/kick/{record['seed']}-original.wav"
                self.assertEqual(Path(copy["path"]), target.resolve())
                self.assertEqual(sf.info(target).subtype, "FLOAT")
                raw, rate = sf.read(record["path"], dtype="float32", always_2d=True)
                actual, actual_rate = sf.read(target, dtype="float32", always_2d=True)
                self.assertEqual(actual_rate, rate)
                np.testing.assert_array_equal(actual, raw)
                self.assertEqual(copy["sha256"], hashlib.sha256(target.read_bytes()).hexdigest())
            with patch.object(stable.trial, "request") as request:
                with self.assertRaises(FileExistsError):
                    stable.run(root, "kick")
                request.assert_not_called()

    def test_original_copy_preserves_rounded_duration_and_fullscale_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            audio = np.full((442367, 1), 1.25, dtype=np.float32)
            audio[0] = -1.25
            result = self.run_trial(root, self.histories(root, audio=audio))
            self.assertEqual(len(result["audition"]["copies"]), 3)
            for copy in result["audition"]["copies"]:
                actual, _ = sf.read(copy["path"], dtype="float32", always_2d=True)
                np.testing.assert_array_equal(actual, audio)
                self.assertGreater(sf.info(copy["path"]).duration, 10)

    def test_rejects_48000_hz_without_copying_or_running_more_seeds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.run_trial(root, self.histories(root, rate=48000))
            self.assertEqual(result["attempts"][0]["status"], "error")
            self.assertEqual(result["unrun_seeds"], [73, 211])
            self.assertEqual(result["audition"]["copies"], [])
            self.assertFalse(list(root.glob("attempts/**/*.wav")))

    def test_failed_middle_seed_stops_and_retains_final_unrun_seed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            replies = self.histories(root)
            replies[3] = {"73": {"status": {"status_str": "error"}, "outputs": {}}}
            result = self.run_trial(root, replies)
            self.assertEqual([item["seed"] for item in result["attempts"]], [17, 73])
            self.assertEqual(result["unrun_seeds"], [211])
            self.assertEqual(result["attempts"][1]["status"], "error")
            self.assertEqual([item["seed"] for item in result["audition"]["copies"]], [17])
            self.assertFalse((root / "attempts/kick/211-prompt.json").exists())

    def test_timeout_keeps_pending_seeds_and_error_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(stable.trial.time, "monotonic", side_effect=[0, 301]):
                result = self.run_trial(Path(directory), [{"prompt_id": "17"}, {}])
            self.assertEqual(result["unrun_seeds"], [73, 211])
            self.assertIn("TimeoutError", result["attempts"][0]["error"])
            self.assertEqual(result["audition"]["copies"], [])

    def test_nonfinite_and_escaped_raw_outputs_are_never_copied(self):
        for invalid in ("nonfinite", "escaped"):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                replies = self.histories(root, audio=np.full((20, 1), np.nan))
                if invalid == "escaped":
                    replies[1]["17"]["outputs"]["7"]["audio"][0]["subfolder"] = "../../../"
                result = self.run_trial(root, replies)
                self.assertEqual(result["attempts"][0]["status"], "error")
                self.assertEqual(result["unrun_seeds"], [73, 211])
                self.assertEqual(result["audition"]["copies"], [])
                self.assertFalse(list(root.glob("attempts/**/*.wav")))

    def test_warmup_is_separate_and_unscored(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            replies = self.histories(root)[:2]
            result = self.run_trial(root, replies, warmup=True)
            self.assertEqual([item["seed"] for item in result["attempts"]], [1009])
            self.assertEqual(result["audition"]["quality"], "not_scored")
            self.assertTrue((root / "smoke/kick/1009-original.wav").is_file())
            prompt = json.loads((root / "smoke/kick/1009-prompt.json").read_text())
            self.assertEqual(prompt["7"]["inputs"]["filename_prefix"], "trial/warmup-kick-1009")
            self.assertFalse((root / "attempts").exists())

    def test_total_time_ends_after_copy_readback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            replies = self.histories(root)
            clock = [0]
            read = stable.sf.read
            def read_with_elapsed_copy(path, *args, **kwargs):
                audio = read(path, *args, **kwargs)
                if str(path).endswith("-original.wav"):
                    clock[0] += 7
                return audio
            with patch.object(stable.time, "perf_counter", side_effect=lambda: clock[0]), patch.object(
                stable.sf, "read", side_effect=read_with_elapsed_copy
            ):
                result = self.run_trial(root, replies)
            self.assertEqual(result["audition"]["seconds"], 21)
            self.assertEqual(result["audition"]["copy_seconds"], 21)
            self.assertEqual(result["audition"]["generation_seconds"], 0)

    def test_copy_never_overwrites_existing_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.histories(root)
            target = root / "existing.wav"
            target.write_bytes(b"retained original")
            with self.assertRaises(FileExistsError):
                stable._copy_original(root / "base/output/trial/17.wav", target, root / "base/output")
            self.assertEqual(target.read_bytes(), b"retained original")

    def test_copy_rejects_readback_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.histories(root)
            target = root / "copy.wav"
            read = stable.sf.read
            def mismatched_readback(path, *args, **kwargs):
                audio, rate = read(path, *args, **kwargs)
                return (audio + np.float32(0.125), rate) if Path(path) == target else (audio, rate)
            with patch.object(stable.sf, "read", side_effect=mismatched_readback):
                with self.assertRaisesRegex(ValueError, "readback differs"):
                    stable._copy_original(root / "base/output/trial/17.wav", target, root / "base/output")

    def test_copy_failure_is_recorded_without_mutating_generation_results(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            replies = self.histories(root)
            with patch.object(stable.sf, "write", side_effect=OSError("copy disk failure")):
                result = self.run_trial(root, replies)
            self.assertTrue(all(item["status"] == "generated" for item in result["attempts"]))
            self.assertEqual(result["audition"]["copies"][0]["status"], "error")
            self.assertIn("copy disk failure", result["audition"]["copies"][0]["error"])
            self.assertEqual(result["audition"]["uncopied_seeds"], [73, 211])


if __name__ == "__main__":
    unittest.main()
