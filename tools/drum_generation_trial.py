"""Frozen YuE2 research trial; never used by the shipped instrument runtime."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request

import numpy as np
import soundfile as sf


URL = "http://127.0.0.1:8189"
MODEL = "yue2_3b_bf16.safetensors"
DESCRIPTIONS = {
    "kick": "A single dry electronic kick drum hit, deep and punchy",
    "snare": "A single dry acoustic snare drum hit, crisp and punchy",
    "hat": "A single dry closed hi-hat hit, short and crisp",
    "percussion": "A single dry hand-played conga hit, warm and percussive",
}
TAIL = (", with a complete natural decay and silence before and after. "
        "Isolated sound, no other instruments, rhythm, music, vocals or effects.")


def matrix():
    return [(kind, seed, text + TAIL) for kind, text in DESCRIPTIONS.items()
            for seed in (17, 73, 211)]


def graph(kind, seed, style):
    def node(name, **inputs):
        return {"class_type": name, "inputs": inputs}

    return {
        "1": node("CheckpointLoaderSimple", ckpt_name=MODEL),
        "2": node("YuE2GenerateMusic", clip=["1", 1], style=style, lyrics="", abc="",
                  mode="full", seed=seed, max_duration=10, temperature=1,
                  top_p=0.95, top_k=100, repetition_penalty=1.2, cfg_scale=1.01),
        "3": node("ConditioningZeroOut", conditioning=["2", 0]),
        "4": node("EmptyYuE2LatentAudio", seconds=["2", 1], batch_size=1),
        "5": node("KSampler", model=["1", 0], positive=["2", 0], negative=["3", 0],
                  latent_image=["4", 0], seed=seed, steps=32, cfg=1,
                  sampler_name="dpm_2", scheduler="sgm_uniform", denoise=1),
        "6": node("VAEDecodeAudio", samples=["5", 0], vae=["1", 2]),
        "7": node("SaveAudioAdvanced", audio=["6", 0],
                  filename_prefix="trial/" + kind + "-" + str(seed), format="flac"),
    }


def request(path, payload=None):
    body = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(URL + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)


def save(path, value):
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def gpu():
    try:
        result = subprocess.run([
            "nvidia-smi", "--query-gpu=timestamp,name,memory.used,memory.free,utilization.gpu",
            "--format=csv,noheader",
        ], capture_output=True, text=True, timeout=10)
        return {"exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
    except Exception as error:
        return {"exit": None, "error": repr(error)}


def prepare(source, destination, first, last, gain):
    destination = Path(destination)
    if destination.exists():
        raise FileExistsError(destination)
    info = sf.info(source)
    if not (isinstance(first, int) and isinstance(last, int)
            and 8000 <= info.samplerate <= 192000
            and 0 <= first < last <= info.frames and 0 < gain <= 16
            and last - first <= 10 * info.samplerate and info.channels in (1, 2)):
        raise ValueError("Invalid crop, gain or audio geometry")
    audio, rate = sf.read(source, start=first, stop=last,
                          dtype="float32", always_2d=True)
    audio = audio * np.float32(gain)
    if not np.isfinite(audio).all() or np.max(np.abs(audio)) >= 1:
        raise ValueError("Nonfinite or clipped prepared audio")
    # Exclusive creation also closes the race between the existence check and write.
    with destination.open("xb") as stream:
        sf.write(stream, audio, rate, format="WAV", subtype="FLOAT")
    check, check_rate = sf.read(destination, dtype="float32", always_2d=True)
    if rate != check_rate or not np.array_equal(check, audio):
        raise ValueError("Prepared audio readback differs")


def run(root, kind, warmup=False, *, graph_builder=None, sample_rate=48000):
    if kind not in DESCRIPTIONS:
        raise ValueError("Unknown trial class")
    root = Path(root).resolve()
    directory = root / ("smoke" if warmup else "attempts") / kind
    directory.mkdir(parents=True, exist_ok=False)
    save(directory / "gpu-before.json", gpu())
    started = time.perf_counter()
    records = []
    cases = ([(kind, 1009, DESCRIPTIONS[kind] + TAIL)] if warmup else
             [case for case in matrix() if case[0] == kind])
    for _, seed, style in cases:
        record = {"class": kind, "seed": seed, "listening": "pending",
                  "distinct_variant": None, "truncated": "unknown"}
        output_name = "warmup-" + kind if warmup else kind
        prompt = (graph if graph_builder is None else graph_builder)(output_name, seed, style)
        save(directory / (str(seed) + "-prompt.json"), prompt)
        tick = time.perf_counter()
        try:
            reply = request("/prompt", {"prompt": prompt})
            save(directory / (str(seed) + "-submission.json"), reply)
            prompt_id = reply["prompt_id"]
            history_path = "/history/" + urllib.parse.quote(prompt_id, safe="")
            deadline = time.monotonic() + 300
            while True:
                history = request(history_path).get(prompt_id)
                if history:
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Owned job exceeded 300 seconds")
                time.sleep(0.1)
            save(directory / (str(seed) + "-history.json"), history)
            outputs = history.get("outputs", {}).get("7", {}).get("audio", [])
            if history.get("status", {}).get("status_str") != "success" or len(outputs) != 1:
                raise ValueError("Job failed or did not save exactly one audio file")
            item = outputs[0]
            output_root = (root / "base/output").resolve()
            source = (output_root / item["subfolder"] / item["filename"]).resolve()
            if item["type"] != "output" or not source.is_relative_to(output_root):
                raise ValueError("Output escaped private output directory")
            info = sf.info(source)
            if (info.channels not in (1, 2) or info.samplerate != sample_rate
                    or not 0 < info.duration <= 10.1):
                raise ValueError("Unexpected raw audio geometry")
            audio, _ = sf.read(source, dtype="float32", always_2d=True)
            if not np.isfinite(audio).all():
                raise ValueError("Nonfinite raw audio")
            record.update(status="generated", path=str(source),
                          sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                          sample_rate=info.samplerate, frames=info.frames)
        except Exception as error:
            record.update(status="error", error=repr(error), requires_owned_server_shutdown=True)
            if isinstance(error, urllib.error.HTTPError):
                record["http_status"] = error.code
                try:
                    body = error.read(65537)
                    record.update(http_body=body[:65536].decode("utf-8", "replace"),
                                  http_body_truncated=len(body) > 65536)
                except Exception as body_error:
                    record["http_body_error"] = repr(body_error)
        record["seconds"] = time.perf_counter() - tick
        save(directory / (str(seed) + "-result.json"), record)
        records.append(record)
        if record.get("requires_owned_server_shutdown"):
            break
    result = {
        "class": kind, "attempts": records,
        "unrun_seeds": [case[1] for case in cases[len(records):]],
        "seconds": time.perf_counter() - started,
        "quality": "not_scored" if warmup else "pending_user_listening",
        "timing_qualified": False,
        "timing_note": "Requires native exclusive lease and reviewed continuous foreign-GPU observations",
    }
    save(directory / "gpu-after.json", gpu())
    save(directory / "summary.json", result)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--class", dest="kind", choices=tuple(DESCRIPTIONS), required=True)
    parser.add_argument("--warmup", action="store_true")
    args = parser.parse_args()
    result = run(args.root, args.kind, args.warmup)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if all(item["status"] == "generated" for item in result["attempts"]) else 1)
