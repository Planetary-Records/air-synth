"""Frozen Comfy Stable Audio 3 Small SFX research condition; no musical scoring."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import time

import numpy as np
import soundfile as sf

_trial_spec = importlib.util.spec_from_file_location(
    "drum_generation_trial", Path(__file__).resolve().with_name("drum_generation_trial.py"))
trial = importlib.util.module_from_spec(_trial_spec)
_trial_spec.loader.exec_module(trial)


def graph(kind, seed, style):
    def node(name, **inputs):
        return {"class_type": name, "inputs": inputs}

    return {
        "1": node("CheckpointLoaderSimple", ckpt_name="stable_audio_3_small_sfx.safetensors"),
        "2": node("CLIPLoader", clip_name="t5gemma_b_b_ul2.safetensors",
                  type="stable_audio", device="default"),
        "3": node("CLIPTextEncode", clip=["2", 0], text=style),
        "8": node("CLIPTextEncode", clip=["2", 0], text=""),
        "4": node("EmptyLatentAudio", seconds=10, batch_size=1),
        "5": node("KSampler", model=["1", 0], positive=["3", 0], negative=["8", 0],
                  latent_image=["4", 0], seed=seed, steps=8, cfg=1,
                  sampler_name="lcm", scheduler="simple", denoise=1),
        "6": node("VAEDecodeAudio", samples=["5", 0], vae=["1", 2]),
        "7": node("SaveAudioAdvanced", audio=["6", 0],
                  filename_prefix="trial/" + kind + "-" + str(seed), format="flac"),
    }


def _copy_original(source, destination, output_root):
    source, destination = Path(source).resolve(), Path(destination)
    if destination.exists():
        raise FileExistsError(destination)
    if not source.is_relative_to(Path(output_root).resolve()):
        raise ValueError("Output escaped private output directory")
    info = sf.info(source)
    if (info.samplerate != 44100 or info.channels not in (1, 2)
            or not 0 < info.duration <= 10.1):
        raise ValueError("Unexpected raw audio geometry")
    audio, rate = sf.read(source, dtype="float32", always_2d=True)
    if not np.isfinite(audio).all():
        raise ValueError("Nonfinite raw audio")
    # Originals retain rounded duration and fullscale values; these are not kept hits.
    with destination.open("xb") as stream:
        sf.write(stream, audio, rate, format="WAV", subtype="FLOAT")
    check, check_rate = sf.read(destination, dtype="float32", always_2d=True)
    if rate != check_rate or not np.array_equal(check, audio):
        raise ValueError("Original WAV readback differs")
    return {"path": str(destination.resolve()), "sample_rate": rate, "frames": len(audio),
            "sha256": hashlib.sha256(destination.read_bytes()).hexdigest()}


def run(root, kind, warmup=False):
    root = Path(root).resolve()
    started = time.perf_counter()
    result = trial.run(root, kind, warmup, graph_builder=graph, sample_rate=44100)
    directory = root / ("smoke" if warmup else "attempts") / kind
    copy_started = time.perf_counter()
    generated = [item for item in result["attempts"] if item["status"] == "generated"]
    copies = []
    for record in generated:
        copy = {"seed": record["seed"]}
        try:
            copy.update(_copy_original(record["path"], directory / f"{record['seed']}-original.wav",
                                       root / "base/output"), status="copied")
        except Exception as error:
            copy.update(status="error", error=repr(error))
        copies.append(copy)
        if copy["status"] == "error":
            break
    finished = time.perf_counter()
    receipt = {
        "class": kind, "copies": copies,
        "uncopied_seeds": [item["seed"] for item in generated[len(copies):]],
        "generation_seconds": result["seconds"], "copy_seconds": finished - copy_started,
        "seconds": finished - started, "quality": result["quality"],
        "timing_qualified": False, "timing_note": result["timing_note"],
        "clock_scope": "Before shared runner (including telemetry) through exact WAV readback and hashing",
    }
    trial.save(directory / "audition.json", receipt)
    return {**result, "audition": receipt}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--class", dest="kind", choices=tuple(trial.DESCRIPTIONS), required=True)
    parser.add_argument("--warmup", action="store_true")
    args = parser.parse_args()
    result = run(args.root, args.kind, args.warmup)
    print(json.dumps(result, indent=2))
    success = (all(item["status"] == "generated" for item in result["attempts"])
               and all(item["status"] == "copied" for item in result["audition"]["copies"])
               and not result["audition"]["uncopied_seeds"])
    raise SystemExit(0 if success else 1)
