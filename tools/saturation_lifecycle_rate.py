"""Generate the isolated, source-bound lifecycle prototype (development only)."""
import argparse
import hashlib
import json
from pathlib import Path
import stat

ROOT = Path(__file__).resolve().parents[1]
HEADER_SHA256 = "3580dde19b51a9237677549732e3078111096a339d3f69667168ccb73d467035"
SOURCE_SHA256 = "69ba286610b2fadd5bcac61f1f1fae8debd98801453c59426ca2b6235d90bc1b"
RATE = b"bool validRate(double rate) noexcept { return std::isfinite(rate) && rate >= 8000 && rate <= 384000; }"
HEADER = b"    void reset() noexcept;"
DECLARATIONS = (b"\r\n    const SaturationSettings& settings() const noexcept;"
                b"\r\n    void processBlock(std::span<Frame>) noexcept;")
RESET = b"void Candidate::reset() noexcept { state->clear(); }\r\n#else"
METHODS = (b"\r\nconst SaturationSettings& Candidate::settings() const noexcept { return state->target; }"
           b"\r\nvoid Candidate::processBlock(std::span<Frame> block) noexcept {"
           b"\r\n    for (auto& frame : block) frame = process(frame);\r\n}")


def identity(data):
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def replace_once(data, before, after):
    if data.count(before) != 1:
        raise ValueError("Expected exactly one complete anchor: " + repr(before))
    return data.replace(before, after, 1)


def originals(header, source):
    if identity(header)["sha256"] != HEADER_SHA256 or identity(source)["sha256"] != SOURCE_SHA256:
        raise ValueError("Unknown original Candidate bytes")


def transform(header, source):
    originals(header, source)
    if source.count(b'#include "SaturationCandidate.h"') != 1:
        raise ValueError("Expected one adjacent generated header include")
    if source.count(RATE) != 1:
        raise ValueError("Expected one complete rate guard")
    generated_header = replace_once(header, HEADER, HEADER + DECLARATIONS)
    generated_source = replace_once(source, RESET, RESET.replace(b"\r\n#else", METHODS + b"\r\n#else"))
    generated_source = replace_once(generated_source, RATE, RATE.replace(b"384000", b"768000"))
    return generated_header, generated_source


def recover(header, source):
    source = replace_once(source, RATE.replace(b"384000", b"768000"), RATE)
    header = replace_once(header, HEADER + DECLARATIONS, HEADER)
    source = replace_once(source, RESET.replace(b"\r\n#else", METHODS + b"\r\n#else"), RESET)
    originals(header, source)
    return header, source


def generate(output):
    output = Path(output).absolute()
    for ancestor in reversed((output, *output.parents)):
        try:
            info = ancestor.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("Redirected output path")
    output = output.resolve()
    build = (ROOT / "build").resolve()
    if not output.is_relative_to(build) or output.name != "saturation-lifecycle-rate" or output == build:
        raise ValueError("Output must be a saturation-lifecycle-rate directory under this worktree build")
    header = (ROOT / "probes/saturation/SaturationCandidate.h").read_bytes()
    source = (ROOT / "probes/saturation/SaturationCandidate.cpp").read_bytes()
    generated = transform(header, source)
    if recover(*generated) != (header, source):
        raise ValueError("Byte-exact transformation reversal failed")
    names = ("SaturationCandidate.h", "SaturationCandidate.cpp")
    descriptor = {
        "schema": 1,
        "original": {name: identity(data) for name, data in zip(names, (header, source))},
        "generated": {name: identity(data) for name, data in zip(names, generated)},
        "transformations": ["unique validRate maximum 384000 to 768000",
                            "public retained-target and ordered-block declarations",
                            "transition state target readback and ordered scalar process loop"],
        "patches": [
            {"file": "SaturationCandidate.cpp", "before": RATE.decode(), "after": RATE.replace(b"384000", b"768000").decode()},
            {"file": "SaturationCandidate.h", "before": HEADER.decode(), "after": (HEADER + DECLARATIONS).decode()},
            {"file": "SaturationCandidate.cpp", "before": RESET.decode(),
             "after": RESET.replace(b"\r\n#else", METHODS + b"\r\n#else").decode()},
        ],
        "reversed_bytes_identical": True,
        "CPU": "DEFERRED", "selected_factor": None, "adoptable": False,
        "production": "FORCED_BYPASS", "numeric_gates": "UNCHANGED", "R4-R7": "OPEN",
    }
    outputs = dict(zip(names, generated))
    outputs["source.json"] = (json.dumps(descriptor, indent=2, sort_keys=True) + "\n").encode()
    for name in outputs:
        path = output / name
        if path.exists() or path.is_symlink():
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError("Nonregular generated output")
            if name in names and path.read_bytes() != outputs[name]:
                raise ValueError("Unknown existing generated Candidate bytes")
    output.mkdir(parents=True, exist_ok=True)
    for name, data in outputs.items():
        path = output / name
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
    return descriptor


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
