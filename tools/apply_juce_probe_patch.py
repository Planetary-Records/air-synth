"""Apply the exact M0 wrapper observation patch; development tooling only."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import subprocess


def apply_patch(source: Path, patch: dict) -> str:
    original = source.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    if digest == patch["post_sha256"]:
        return "already_applied"
    if digest != patch["pre_sha256"]:
        raise ValueError(f"JUCE wrapper preimage mismatch: {digest}; source left unchanged")
    result = original
    for old, new in patch["replacements"]:
        old, new = old.encode(), new.encode()
        if result.count(old) != 1:
            raise ValueError("JUCE patch context must occur exactly once; source left unchanged")
        result = result.replace(old, new)
    if hashlib.sha256(result).hexdigest() != patch["post_sha256"]:
        raise ValueError("JUCE patch postimage mismatch; source left unchanged")
    temporary = source.with_suffix(source.suffix + ".probe-tmp")
    temporary.write_bytes(result)
    temporary.replace(source)
    return "applied"


def self_test():
    class PatchTests(unittest.TestCase):
        def test_guarded_idempotent_apply(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "wrapper.cpp"
                path.write_bytes(b"before\n")
                patch = {"pre_sha256": hashlib.sha256(b"before\n").hexdigest(),
                         "post_sha256": hashlib.sha256(b"after\n").hexdigest(),
                         "replacements": [["before", "after"]]}
                self.assertEqual(apply_patch(path, patch), "applied")
                self.assertEqual(path.read_bytes(), b"after\n")
                self.assertEqual(apply_patch(path, patch), "already_applied")

        def test_mismatch_has_no_mutation(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "wrapper.cpp"
                path.write_bytes(b"unexpected")
                with self.assertRaisesRegex(ValueError, "preimage"):
                    apply_patch(path, {"pre_sha256": "bad", "post_sha256": "bad"})
                self.assertEqual(path.read_bytes(), b"unexpected")

        def test_bad_patch_has_no_mutation(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "wrapper.cpp"
                path.write_bytes(b"before\n")
                patch = {"pre_sha256": hashlib.sha256(b"before\n").hexdigest(),
                         "post_sha256": "wrong", "replacements": [["before", "after"]]}
                with self.assertRaisesRegex(ValueError, "postimage"):
                    apply_patch(path, patch)
                self.assertEqual(path.read_bytes(), b"before\n")
                patch["replacements"] = [["missing", "after"]]
                with self.assertRaisesRegex(ValueError, "exactly once"):
                    apply_patch(path, patch)
                self.assertEqual(path.read_bytes(), b"before\n")

    return unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(PatchTests)).wasSuccessful()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--juce-root", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        raise SystemExit(0 if self_test() else 1)
    if args.juce_root is None:
        parser.error("--juce-root is required")
    spec = json.loads((Path(__file__).resolve().parents[1] / "probes/native/juce-patches/host-observation.json").read_text())
    head = subprocess.run(["git", "-C", str(args.juce_root), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
    if head != spec["juce_commit"]:
        raise SystemExit("JUCE commit mismatch; source left unchanged")
    print(apply_patch(args.juce_root / spec["relative_path"], spec))
