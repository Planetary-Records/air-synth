"""Compare Era Reverb with ValhallaVintageVerb from the actual-VST3 packet (development tooling only).

`RackSynthRender --era-reverb` renders each comparison source dry and each row processed through AI Rack Synth.
This tool feeds the same dry source through VintageVerb with pedalboard at the row's closest settings, measures both
outputs with the VintageVerb investigation's methods and records them beside the spec's tolerance. It also renders the
four listening demos' dry WAVs through VintageVerb at the Small Hours recipe's closest settings. Not a parity claim.

    python tools/era_reverb_comparison.py --packet ABS_ERA_PACKET --output FRESH_ABS_DIR
"""
import argparse
import ast
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
ROWS = ROOT / "tools/era_reverb_comparison_rows.json"
MEASURE = ROOT / "docs/verification/2026-10-22-vintageverb-investigation/scripts/measure.py"
PLUGIN = r"C:\Program Files\Common Files\VST3\Valhalla DSP\ValhallaVintageVerb.vst3"
SOURCES = {"click", "burst", "sine220", "sine220_quiet", "sine1k"}
METRICS = {"onset_ms", "harmonics_db", "attack_ms", "early_late_db", "rt60_mid_s", "rt60_mid_and_density", "density_ms",
           "rt60_octaves_s", "mod_width_hz", "tail_bandwidth_hz", "gate_span_ms", "gate_shape_db", "filter_response_db",
           "iacc_tail", "dry_gain"}
ERA_KEYS = {"voicing", "age", "decay_seconds", "predelay_ms", "size", "attack", "diffusion", "wobble", "wobble_rate_hz",
            "grit", "bass_multiplier", "damping_hz", "low_cut_hz", "high_cut_hz", "mix"}
# Output-filter rows read their response against the same settings without the filter; mix rows split into the dry
# source and the same settings at Mix 1, because Era's Hall answers within the dry-gain window.
REFERENCES = {"filter_response_db": "decay-hall-4", "dry_gain": "size-hall-1"}
# The Small Hours recipe's closest VintageVerb settings: Random Space in the 1970s colour, Damping 5 kHz as the
# in-loop high shelf, Wobble 0.4 at 0.7 Hz as depth 40 % and rate 0.7 Hz.
DEMO_SETTINGS = dict(mode="Random Space", colour="seventies", mix=30, predelay=25, decay=8.0, size=85, attack=60,
                     bassmult=1.2, bassxover=300, highshelf=-24, highfreq=5000, earlydiffusion=100, latediffusion=100,
                     modrate=0.7, moddepth=40, highcut=9000, lowcut=150)
DEMOS = ("pad", "chop", "bell", "snare")
# Only the investigation's constants and functions are taken; its measurement loops never run.
KEPT_NAMES = {"RATE", "BLOCK", "OCTAVES", "CHOICES", "_memo"}


def load_rows(path=ROWS):
    data = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    if data.get("rate") != 48000 or data.get("block") != 512:
        raise ValueError("comparison rows must be at 48 kHz in 512-sample blocks")
    if set(data.get("neutral", {})) != ERA_KEYS:
        raise ValueError("neutral settings must name every Era Reverb field")
    if set(data.get("sources", {})) != SOURCES:
        raise ValueError("sources must describe exactly the stimuli the renderer plays")
    seen = set()
    for row in data.get("rows", []):
        if row.get("id") in seen or not row.get("id"):
            raise ValueError(f"row id missing or repeated: {row.get('id')}")
        seen.add(row["id"])
        if row.get("source") not in SOURCES or row.get("metric") not in METRICS:
            raise ValueError(f"{row['id']}: unknown source or metric")
        if not 3 <= float(row.get("seconds", 0)) <= 30:
            raise ValueError(f"{row['id']}: seconds must be 3 to 30")
        if not set(row.get("ours", {})) <= ERA_KEYS:
            raise ValueError(f"{row['id']}: unknown Era Reverb field")
        vv = row.get("vintageverb", {})
        if not {"mode", "colour"} <= set(vv) or not set(vv) - {"mode", "colour"} <= set(data["vintageverb_neutral"]):
            raise ValueError(f"{row['id']}: VintageVerb settings need a mode, a colour and known parameters")
        if not row.get("tolerance"):
            raise ValueError(f"{row['id']}: tolerance text missing")
    if not set(REFERENCES.values()) <= seen:
        raise ValueError("a reference row is missing")
    return data


def methods_source(path=MEASURE):
    """The investigation's imports, constants and function definitions, without its top-level measurement code."""
    tree = ast.parse(pathlib.Path(path).read_text(encoding="utf-8"))
    def kept(node):
        if isinstance(node, (ast.Import, ast.ImportFrom, ast.FunctionDef)):
            return True
        if isinstance(node, ast.Assign):
            names = {n.id for t in node.targets for n in ast.walk(t) if isinstance(n, ast.Name)}
            return bool(names) and names <= KEPT_NAMES
        return False
    return ast.Module(body=[node for node in tree.body if kept(node)], type_ignores=[])


def load_methods(rows, plugin_path=PLUGIN):
    import pedalboard
    namespace = {"__name__": "vintageverb_investigation_methods"}
    exec(compile(methods_source(), str(MEASURE), "exec"), namespace)
    namespace["plugin"] = pedalboard.load_plugin(plugin_path)
    namespace["P"] = namespace["plugin"].parameters
    namespace["BASE"] = dict(rows["vintageverb_neutral"])
    return namespace


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def read_wav(path):
    from pedalboard.io import AudioFile
    import numpy as np
    with AudioFile(str(path)) as f:
        if f.samplerate != 48000 or f.num_channels != 2:
            raise ValueError(f"{path}: expected 48 kHz stereo")
        return f.read(f.frames).astype(np.float64)


def write_wav(path, y):
    from pedalboard.io import AudioFile
    import numpy as np
    with AudioFile(str(path), "w", 48000, 2, bit_depth=32) as f:
        f.write(np.asarray(y, np.float32))
    return {"name": path.name, "sha256": sha256(path)}


def vintageverb(m, source, seconds, settings):
    """Process like the investigation: configure, settle on a second of silence after a reset, then render."""
    import numpy as np
    settings = dict(settings)
    shown = m["configure"](mode=settings.pop("mode"), colour=settings.pop("colour"), **settings)
    rate, block, plugin = m["RATE"], m["BLOCK"], m["plugin"]
    plugin.process(np.zeros((2, rate), np.float32), rate, buffer_size=block, reset=True)
    x = np.zeros((2, int(round(seconds * rate))), np.float32)
    n = min(source.shape[1], x.shape[1])
    x[:, :n] = source[:, :n]
    return plugin.process(x, rate, buffer_size=block, reset=False).astype(np.float64), shown


def level_near(f, db, hz, bins=3):
    import numpy as np
    i = int(np.argmin(np.abs(f - hz)))
    return float(np.max(db[max(0, i - bins):i + bins + 1]))


def band_level(f, db, hz):
    """Mean power over a sixth of an octave around hz, in dB."""
    import numpy as np
    sel = (f >= hz * 2 ** (-1 / 12)) & (f <= hz * 2 ** (1 / 12))
    return float(10 * np.log10(np.mean(10 ** (db[sel] / 10))))


def deconvolve(y, dry, regularisation=1e-3):
    """The click is a 3 ms noise burst, not an impulse; this recovers each channel's impulse response."""
    import numpy as np
    n = 1 << int(np.ceil(np.log2(y.shape[1] + dry.shape[1])))
    x = np.fft.rfft(dry.mean(axis=0), n)
    floor = regularisation * np.max(np.abs(x) ** 2)
    return np.array([np.fft.irfft(np.fft.rfft(ch, n) * np.conj(x) / (np.abs(x) ** 2 + floor), n)[:y.shape[1]] for ch in y])


def diffuse_ms(m, y):
    """The module's rule (tests/native/ReverbAnalysis.h): the investigation's NED, skipping windows 60 dB under the peak."""
    import numpy as np
    from math import erfc, sqrt
    rate = m["RATE"]
    h = (y[0] + y[1])[m["onset"](y):]
    half, floor = int(rate * 0.02) // 2, 1e-3 * np.max(np.abs(h))
    for c in range(half, min(len(h) - half, int(rate * 0.6)), rate // 1000):
        w = h[c - half:c + half]
        deviation = np.std(w)
        if deviation > floor and np.mean(np.abs(w) > deviation) / erfc(1 / sqrt(2)) >= 0.9:
            return c / rate * 1000
    return None


def measure(m, metric, y, dry, reference=None):
    import numpy as np
    rate = m["RATE"]
    t0 = m["onset"](y)
    if metric == "onset_ms":
        return {"onset_ms": (t0 - m["onset"](dry)) / rate * 1000}
    if metric == "harmonics_db":
        f, db = m["spectrum"](y, 0.5, 1.8, 16384)
        f0 = level_near(f, db, 220)
        return {"fundamental_db": f0, "h2_db": level_near(f, db, 440) - f0, "h3_db": level_near(f, db, 660) - f0,
                "reverb_transfer_removed": False}
    if metric == "attack_ms":
        env = m["envelope_db"](y, 10)
        peak = int(np.argmax(env[:int(rate * 1.5)]))
        return {"attack_ms": (peak - t0) / rate * 1000, "attack_from_input_ms": (peak - m["onset"](dry)) / rate * 1000}
    if metric == "early_late_db":
        # Reflections quieter than the onset threshold would move a processed onset into the tail.
        t0 = m["onset"](dry)
        env = m["envelope_db"](y, 10)
        early = float(np.max(env[t0:t0 + int(0.05 * rate)]))
        late_window = env[t0 + int(0.1 * rate):t0 + int(0.6 * rate)]
        tail = env[t0 + int(0.05 * rate):t0 + rate]
        return {"early_db": early, "late_db": float(np.max(late_window)), "late_minus_early_db": float(np.max(late_window)) - early,
                "tail_peak_ms": (int(np.argmax(tail)) + int(0.05 * rate)) / rate * 1000}
    if metric == "rt60_mid_s":
        return {"rt60_mid_s": m["rt60"](m["band"](y, 1000))}
    if metric in ("rt60_mid_and_density", "density_ms"):
        ir, response = m["describe_ir"](y), deconvolve(y, dry)
        return {"rt60_mid_s": ir["rt60_mid_s"], "density_ms": ir["ned_diffuse_ms"],
                "density_deconvolved_ms": m["describe_ir"](response)["ned_diffuse_ms"],
                "density_deconvolved_floor_ms": diffuse_ms(m, response)}
    if metric == "rt60_octaves_s":
        octaves = {fc: m["rt60"](m["band"](y, fc)) for fc in m["OCTAVES"]}
        ratio = lambda a, b: None if not octaves[a] or not octaves[b] else octaves[a] / octaves[b]
        return {"rt60_octaves_s": octaves, "ratio_8k_1k": ratio(8000, 1000), "ratio_8k_500": ratio(8000, 500),
                "ratio_63_1k": ratio(63, 1000)}
    if metric == "mod_width_hz":
        bandwidth, outside, _, _ = m["mod_width"](y, 2.3, 4.3, 1000)
        return {"mod_width_hz": bandwidth, "energy_outside_5hz": outside}
    if metric == "tail_bandwidth_hz":
        start = t0 / rate + 0.2
        f, db = m["spectrum"](y, start, start + 0.8, 1024)
        return {"tail_bandwidth_hz": m["bandwidth_20db"](f, db)}
    if metric in ("gate_span_ms", "gate_shape_db"):
        env = m["envelope_db"](y, 10)
        inside = np.where(env >= np.max(env) - 20)[0]
        first, last = int(inside[0]), int(inside[-1])
        span = env[first:last + 1]
        tenth = max(1, len(span) // 10)
        return {"gate_span_ms": (last - first) / rate * 1000, "start_db": float(np.mean(span[:tenth])),
                "end_db": float(np.mean(span[-tenth:])), "end_minus_start_db": float(np.mean(span[-tenth:]) - np.mean(span[:tenth])),
                "peak_position": float(np.argmax(span) / max(1, len(span) - 1))}
    if metric == "filter_response_db":
        result = {}
        for name, (start, stop) in (("early", (0.03, 0.33)), ("late", (1.5, 2.5))):
            f, db = m["spectrum"](y, start, stop, 4096)
            _, ref = m["spectrum"](reference, start, stop, 4096)
            result[name] = {str(hz): band_level(f, db, hz) - band_level(f, ref, hz) for hz in (50, 300, 1000, 4000, 20000)}
        return result
    if metric == "iacc_tail":
        return {"iacc_tail": m["iacc"](y), "iacc_tail_deconvolved": m["iacc"](deconvolve(y, dry))}
    if metric == "dry_gain":
        o = m["onset"](dry)
        d, w = dry[:, o:o + int(0.0015 * rate)], y[:, o:o + int(0.0015 * rate)]
        n = min(y.shape[1], dry.shape[1], reference.shape[1])
        basis = np.stack([dry[:, :n].ravel(), reference[:, :n].ravel()], axis=1)
        gains = np.linalg.lstsq(basis, y[:, :n].ravel(), rcond=None)[0]
        return {"dry_gain": float(np.sum(w * d) / np.sum(d * d)), "dry_gain_wet_removed": float(gains[0]),
                "wet_gain": float(gains[1])}
    raise ValueError(metric)


def check_packet(packet):
    report = json.loads((packet / "era-reverb-report.json").read_text(encoding="utf-8"))
    if report.get("success") is not True:
        raise ValueError("the Era Reverb packet did not succeed")
    return report


def run(packet, output, plugin_path=PLUGIN):
    import numpy as np
    rows = load_rows()
    report = check_packet(packet)
    if output.exists():
        raise ValueError("output must be fresh")
    output.mkdir(parents=True)
    m = load_methods(rows, plugin_path)
    sources = {name: read_wav(packet / "comparison" / f"source-{name}.wav") for name in {row["source"] for row in rows["rows"]}}
    outputs, table = {}, []
    for row in rows["rows"]:
        dry = sources[row["source"]]
        ours = read_wav(packet / "comparison" / f"{row['id']}-processed.wav")
        theirs, shown = vintageverb(m, dry, float(row["seconds"]), row["vintageverb"])
        wav = write_wav(output / f"{row['id']}-vintageverb.wav", theirs)
        outputs[row["id"]] = (ours, theirs)
        frames = ours.shape[1]
        trimmed = dry[:, :frames] if dry.shape[1] >= frames else np.pad(dry, ((0, 0), (0, frames - dry.shape[1])))
        reference = outputs.get(REFERENCES.get(row["metric"]), (None, None))
        entry = {"id": row["id"], "behaviour": row["behaviour"], "metric": row["metric"], "tolerance": row["tolerance"],
                 "ours_settings": {**rows["neutral"], **row["ours"]}, "vintageverb_settings": shown, "vintageverb_wav": wav,
                 "ours": measure(m, row["metric"], ours, trimmed, reference[0]),
                 "vintageverb": measure(m, row["metric"], theirs, trimmed[:, :theirs.shape[1]], reference[1])}
        table.append(entry)
        print(f"ERA_COMPARE {row['id']} ours={json.dumps(entry['ours'], default=float)} vintageverb={json.dumps(entry['vintageverb'], default=float)}", flush=True)
    demos, listening = [], {entry["scene"]: entry for entry in report["listening_packet"]}
    for scene in DEMOS:
        # VintageVerb gets the raw dry render, the level Era received; its listening copy takes the packet's gain.
        dry = read_wav(packet / f"{scene}-dry.wav")
        theirs, shown = vintageverb(m, dry, dry.shape[1] / 48000, DEMO_SETTINGS)
        gain_db = float(listening[scene]["listen_gain_db"])
        loud = theirs * 10 ** (gain_db / 20)
        if np.max(np.abs(loud)) >= 1:
            raise ValueError(f"the {scene} VintageVerb listening copy clips at {gain_db} dB")
        named = lambda name: {"name": name, "sha256": sha256(packet / name)}
        demos.append({"scene": scene, "dry": named(f"{scene}-dry.wav"), "ours": named(f"{scene}-processed.wav"),
                      "dry_listen": named(f"{scene}-dry-listen.wav"), "ours_listen": named(f"{scene}-processed-listen.wav"),
                      "vintageverb": write_wav(output / f"{scene}-vintageverb.wav", theirs),
                      "vintageverb_listen": write_wav(output / f"{scene}-vintageverb-listen.wav", loud),
                      "listen_gain_db": gain_db, "vintageverb_settings": shown})
    result = {"scope": "Development evidence through the actual VST3 and VintageVerb at matched settings; not a parity claim",
              "listening": "DEFERRED_BY_USER", "rows_file": {"path": str(ROWS.relative_to(ROOT)), "sha256": sha256(ROWS)},
              "methods": {"path": str(MEASURE.relative_to(ROOT)), "sha256": sha256(MEASURE)},
              "packet": {"path": str(packet), "module": report["module_after"], "renderer": report["renderer_after"]},
              "vintageverb": plugin_path, "rows": table, "demos": demos}
    (output / "comparison.json").write_text(json.dumps(result, indent=1, default=float), encoding="utf-8")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--packet", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--plugin", default=PLUGIN)
    args = parser.parse_args(argv)
    if not args.packet.is_absolute() or not args.output.is_absolute():
        parser.error("--packet and --output must be absolute")
    result = run(args.packet, args.output, args.plugin)
    print(f"SUCCESS Era Reverb comparison: {len(result['rows'])} rows, {len(result['demos'])} demos through VintageVerb; listening DEFERRED_BY_USER")
    return 0


if __name__ == "__main__":
    sys.exit(main())
