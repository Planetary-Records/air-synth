"""Independently validate one completed export, then seal its provisional portable bytes."""
import csv
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def write_json(p, value):
    p.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

def tlog_text(p):
    data = p.read_bytes()
    return data.decode("utf-16" if data.startswith(b"\xff\xfe") else "utf-16-le" if b"\0" in data else "utf-8-sig")

def seal(output, original_hashes, export_name="export", build_name="producer-build", export_directory="export"):
    source = output / export_directory
    destination = Path(__file__).parent / "fixtures/candidate-20261021"
    assert not destination.exists(), "refuse resealing or overwriting expected data"
    export = json.loads((output / f"commands/{export_name}.json").read_text())
    assert export["exit"] == 0
    assert "checks=391 failures=0" in (output / f"commands/{export_name}.stdout").read_text()
    old = json.loads(original_hashes.read_text())["files"]
    originals = {k: v for k, v in old.items() if k != "manifest.json"}
    for name, entry in originals.items():
        assert (source / name).stat().st_size == entry["bytes"] and sha(source / name) == entry["sha256"], name
    with (source / "settled-settings.csv").open(newline="") as stream:
        rows = list(csv.reader(stream))
    globals_header = "mode,input_trim,output_trim,mix,compensation,low_hz,high_hz,pre_on,pre_hz,post_on,post_hz".split(",")
    lane_header = [f"lane{i}_{field}" for i in range(4) for field in ("bypass", "character", "drive", "tilt", "mix", "trim")]
    assert rows[0] == ["case", "file", "rate", "factor", *globals_header, *lane_header]
    assert len(rows) == 19
    expected_names = set(originals) | {"settled-settings.csv"}
    index = 0
    for rate in (44100, 48000, 96000):
        for mode in range(2):
            for character in range(3):
                name = f"settled-{rate}-{mode}-{character}.f64"
                row = rows[index + 1]
                assert len(row) == 39
                assert row[:4] == [str(index), name, str(rate), "4"]
                expected = [mode, 0, 0, .75, 0, 180, 3500, 0, 20, 0, 20000] + [0, character, 12, 0, .75, 0] * 4
                assert list(map(float, row[4:])) == expected
                expected_names.add(name)
                index += 1
    assert {p.name for p in source.iterdir()} == expected_names
    for p in source.glob("*.f64"):
        data = p.read_bytes()
        assert len(data) == 32768 and all(math.isfinite(x) for x in struct.unpack("<4096d", data)), p.name
    # Recheck every fixed CSV's shape and intentional refusal record independently of C++.
    for name, prefix, count in (("events-settings.csv", ["frame", "operation", "admitted", "rate", "factor"], 32), ("serial-settings.csv", ["frame", "slot"], 11)):
        with (source / name).open(newline="") as stream:
            parsed = list(csv.reader(stream))
        assert parsed[0] == prefix + globals_header + lane_header and len(parsed) == count + 1
        for row in parsed[1:]:
            assert len(row) == len(parsed[0]) and 0 <= int(row[0]) < 2048
        assert all(int(a[0]) <= int(b[0]) for a, b in zip(parsed[1:], parsed[2:]))
    with (source / "serial-events.csv").open(newline="") as stream:
        parsed = list(csv.reader(stream))
    assert parsed[0] == ["frame", "operation", "slot", "bypassed"] and len(parsed) == 10 and all(len(row) == 4 for row in parsed)
    image = Path(export["command"][-3])
    assert sha(image) == export["image_sha256"]
    shutil.copyfile(image, output / "producer.exe")
    inputs = [ROOT / "CMakeLists.txt", ROOT / "CMakePresets.json", Path(__file__), ROOT / "probes/saturation/contracts/SaturationContractTests.cpp"]
    inputs += [ROOT / "probes/saturation" / name for name in ("SaturationCandidate.cpp", "SaturationCandidate.h", "SaturationMath.h", "SaturationGain.h", "SaturationCalibration.h", "SaturationWideNetwork.cpp", "SaturationWideNetwork.h")]
    build = ROOT / "build/sm20/current"
    commands = {}
    links = {}
    for p in (build / "RackSynthSaturationContractTests.dir/Debug").rglob("*.command.1.tlog"):
        commands[p.relative_to(build).as_posix()] = {"sha256": sha(p), "text": tlog_text(p)}
    for p in (build / "RackSynthSaturationContractTests.dir/Debug").rglob("link.read.1.tlog"):
        for line in tlog_text(p).splitlines():
            item = Path(line)
            if item.is_file():
                links[str(item)] = sha(item)
    receipt = {"status": "UNADOPTED", "CPU": "DEFERRED", "selected_factor": None, "production": "FORCED_BYPASS", "gates": "UNCHANGED", "probe_factor": 4,
               "image_sha256": sha(image), "source_inputs": {p.relative_to(ROOT).as_posix(): sha(p) for p in inputs}, "commands": commands, "link_inputs": links,
               "build_receipt_sha256": sha(output / f"commands/{build_name}.json"), "export": export,
               "export_stdout_sha256": sha(output / f"commands/{export_name}.stdout"), "export_stderr_sha256": sha(output / f"commands/{export_name}.stderr")}
    destination.mkdir(parents=True)
    for p in source.iterdir():
        shutil.copyfile(p, destination / p.name)
    write_json(destination / "producer-receipt.json", receipt)
    manifest = {"schema": 1, "status": "UNADOPTED", "CPU": "DEFERRED", "selected_factor": None, "production": "FORCED_BYPASS", "gates": "UNCHANGED", "probe_factor": 4,
                "frames": 2048, "channels": 2, "format": "IEEE754_f64_little_endian_interleaved_stereo",
                "partitions": [1, 17, 64, 127, 512], "common_cases": 18,
                "initial_sequence": "fresh Candidate; setSettings; prepare(rate,4); reset; process saved input; no edits or warmup",
                "reset": "zero audio histories and settled admitted targets",
                "common_reference": "settled candidate common behavior only; UNADOPTED; requalify/regenerate after adoption before production use",
                "characterization": "event/serial current candidate only: immediate structural/style switches, effective 8..384kHz; 384001/768000 refusals are NOT future production behavior",
                "original_task20_archive_sha256": json.loads(original_hashes.read_text())["archive_sha256"],
                "producer_image_sha256": sha(image), "files": {p.name: {"bytes": p.stat().st_size, "sha256": sha(p)} for p in sorted(destination.iterdir())}}
    write_json(destination / "manifest.json", manifest)
    write_json(output / "seal.json", {"manifest_sha256": sha(destination / "manifest.json"), "original_six_byte_exact": True, "audio_files": 21, "audio_bytes_each": 32768, "common_rows": 18, "files": manifest["files"]})
    print(f"sealed {len(manifest['files'])} payload files; manifest pin {sha(destination / 'manifest.json')}")

if __name__ == "__main__":
    seal(Path(sys.argv[1]), Path(sys.argv[2]), *sys.argv[3:])
