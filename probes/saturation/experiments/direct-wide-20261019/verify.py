"""Pure local byte verification of this source snapshot; never launches subprocesses."""
import hashlib
import json
from pathlib import Path


def main():
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / "manifest.json").read_text(encoding="utf-8"))
    expected = manifest["files"]
    actual = {p.relative_to(here).as_posix() for p in here.rglob("*") if p.is_file()}
    if actual != set(expected) | {"manifest.json"}:
        raise SystemExit("FAIL: source snapshot file membership differs")
    for name, digest in expected.items():
        path = (here / name).resolve()
        if not path.is_relative_to(here) or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise SystemExit("FAIL: source hash differs: " + name)
    for name in manifest["changedVariantFiles"]:
        if expected["baseline/" + name] == expected["proposed/" + name]:
            raise SystemExit("FAIL: expected variant distinction missing: " + name)
    for name in manifest["sharedFiles"]:
        source = manifest["origins"]["shared/" + name]
        if expected["shared/" + name] != source["sha256"]:
            raise SystemExit("FAIL: shared origin mismatch: " + name)
    if expected["baseline/SaturationCandidate.cpp"] != manifest["baselineCandidateSha256"]:
        raise SystemExit("FAIL: ECC baseline identity differs")
    if expected["proposed/SaturationCandidate.cpp"] != manifest["proposedThreeFiles"]["SaturationCandidate.cpp"]:
        raise SystemExit("FAIL: proposal identity differs")
    print("PASS: " + str(len(expected)) + " exact source/document hashes; no native qualification")


if __name__ == "__main__":
    main()
