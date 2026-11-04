"""Development-only Codex app-server MCP discovery probe; never starts a turn."""
import argparse
import base64
import math
import struct
from contextlib import contextmanager
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import uuid

class ProbeError(RuntimeError):
    pass

class ChildTransport:
    def __init__(self, command, *, env=None, cwd=None, stderr=None):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=stderr or subprocess.DEVNULL, env=env, cwd=cwd)
        self.incoming = queue.Queue(maxsize=64)
        self.responses = {}
        self.notifications = deque(maxlen=256)
        self.sent_methods = []
        self.stopped = threading.Event()
        self.reader = threading.Thread(target=self._read, name="app-server-stdout")
        self.reader.start()

    def _read(self):
        try:
            while not self.stopped.is_set():
                line = self.process.stdout.readline(8388609)
                if not line:
                    item = ProbeError("app-server stdout closed")
                elif len(line) > 8388608 or not line.endswith(b"\n"):
                    item = ProbeError("oversized or truncated app-server line")
                else:
                    item = json.loads(line)
                while not self.stopped.is_set():
                    try:
                        self.incoming.put(item, timeout=.05)
                        break
                    except queue.Full:
                        continue
                if isinstance(item, ProbeError):
                    break
        except Exception as error:
            try:
                self.incoming.put_nowait(ProbeError(str(error)))
            except queue.Full:
                pass

    def send(self, value):
        self.sent_methods.append(value.get("method"))
        try:
            self.process.stdin.write((json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8"))
            self.process.stdin.flush()
        except (OSError, ValueError) as error:
            raise ProbeError("app-server stdin closed") from error

    def receive(self, ident, timeout=10):
        deadline = time.monotonic() + timeout
        while ident not in self.responses:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProbeError(f"timeout waiting for response {ident}")
            try:
                item = self.incoming.get(timeout=remaining)
            except queue.Empty as error:
                raise ProbeError(f"timeout waiting for response {ident}") from error
            if isinstance(item, Exception):
                raise item
            if not isinstance(item, dict):
                raise ProbeError("non-object app-server message")
            if "id" in item:
                if len(self.responses) >= 64:
                    raise ProbeError("too many unmatched app-server responses")
                self.responses[item["id"]] = item
            else:
                self.notifications.append(item)
        response = self.responses.pop(ident)
        if "error" in response:
            raise ProbeError(f"app-server request {ident} failed: {response['error']}")
        if "result" not in response:
            raise ProbeError(f"app-server request {ident} has no result")
        return response

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.stopped.set()
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.reader.join(timeout=3)
        self.process.stdout.close()
        if self.reader.is_alive():
            raise ProbeError("app-server stdout worker did not retire")

def wait_inventory(client, thread_id, timeout):
    deadline = time.monotonic() + timeout
    ident = 3
    while time.monotonic() < deadline:
        client.send({"id": ident, "method": "mcpServerStatus/list", "params": {
            "serverName": "air_mcp_fixture", "threadId": thread_id, "detail": "full"}})
        inventory = client.receive(ident, max(.01, deadline - time.monotonic()))["result"]
        for server in inventory.get("data", []):
            if server.get("name") != "air_mcp_fixture":
                raise ProbeError("unexpected inherited MCP server")
            status = server.get("runtimeStatus")
            if status in ("failed", "cancelled", "disabled", "authenticationRequired") or server.get("toolsError"):
                raise ProbeError("fixture MCP startup failed")
            if status == "connected":
                tools = server.get("tools", {})
                if not {"list_instances", "get_state"}.issubset(tools):
                    raise ProbeError("connected fixture has no required tool inventory")
                if not server.get("serverInfo") or "tools" not in (server.get("serverCapabilities") or {}):
                    raise ProbeError("missing advertised server identity/capabilities")
                for name in ("list_instances", "get_state"):
                    tool = tools[name]
                    if tool.get("name") != name or tool.get("inputSchema", {}).get("type") != "object":
                        raise ProbeError("invalid advertised tool schema")
                return server
        ident += 1
        time.sleep(min(.1, max(0, deadline - time.monotonic())))
    raise ProbeError("fixture MCP startup timed out")

def tool_call(client, thread_id, ident, tool, arguments, evidence):
    client.send({"id": ident, "method": "mcpServer/tool/call", "params": {
        "server": "air_mcp_fixture", "threadId": thread_id, "tool": tool, "arguments": arguments}})
    result = client.receive(ident)["result"]
    evidence.append({"tool": tool, "arguments": arguments, "result": result})
    if result.get("isError", False) or not isinstance(result.get("structuredContent"), dict):
        raise ProbeError(f"{tool} failed or has no structuredContent")
    data = result["structuredContent"]
    if data.get("status") != "ready":
        raise ProbeError(f"{tool} did not return ready")
    return data

def edit_workflow(client, thread_id, evidence):
    ident = 11000
    def call(tool, arguments):
        nonlocal ident
        ident += 1
        return tool_call(client, thread_id, ident, tool, arguments, evidence)
    listed = call("list_instances", {})
    instances = listed.get("instances", [])
    if len(instances) != 2 or instances[0]["instance_id"] == instances[1]["instance_id"]:
        raise ProbeError("edit fixture needs two distinct actual processors")
    target = {"instance_id": instances[0]["instance_id"]}
    other = {"instance_id": instances[1]["instance_id"]}
    before = call("get_state", target)
    b_before = call("get_state", other)
    catalog = call("get_catalog", target)
    if not isinstance(catalog.get("edits_schema"), dict):
        raise ProbeError("native catalog has no exact edit schema")
    def values(state):
        rows = state.get("values")
        if not isinstance(rows, list) or not rows:
            raise ProbeError("missing actual state values")
        return {row["id"]: row["normalized"] for row in rows}
    expected = values(before)
    value = .23 if abs(expected["global.master_level"] - .23) > .000001 else .37
    staged = call("prepare_edit", {**target, "snapshot": before["snapshot"], "policy": {},
                  "edits": {"parameter_edits": [{"id": "global.master_level", "normalized": value}]}})
    if not isinstance(staged.get("edit_id"), str) or not staged.get("diff"):
        raise ProbeError("prepare returned no actual staged edit")
    edit = {**target, "edit_id": staged["edit_id"]}
    call("get_edit", edit)
    if values(call("get_state", target)) != expected:
        raise ProbeError("prepare changed committed sound")
    first = call("apply_edit", edit)
    if not first.get("changed") or not isinstance(first.get("transaction_id"), str) or not first["transaction_id"].isdigit() or int(first["transaction_id"]) == 0:
        raise ProbeError("apply returned no native history transaction")
    if call("apply_edit", edit) != first:
        raise ProbeError("Apply replay did not return the original receipt")
    committed = call("get_state", target)
    actual = values(committed)
    def stable_fields(state):
        return {key: value for key, value in state.items() if key not in ("values", "snapshot", "revision")}
    if stable_fields(committed) != stable_fields(before):
        raise ProbeError("Apply changed unrelated committed fields")
    changed = [key for key in expected if actual.get(key) != expected[key]]
    if changed != ["global.master_level"] or abs(actual["global.master_level"] - value) > .000001 or committed["revision"] != first["revision"]:
        raise ProbeError("Apply changed the wrong target/fields or returned the wrong revision")
    if call("get_state", other) != b_before:
        raise ProbeError("Apply changed the other processor")
    history = {**target, "transaction_id": first["transaction_id"], "expected_revision": first["revision"]}
    undone = call("undo_edit", history)
    if values(call("get_state", target)) != expected or undone["revision"] == first["revision"]:
        raise ProbeError("undo did not restore actual state with a new revision")
    redone = call("redo_edit", {**history, "expected_revision": undone["revision"]})
    if values(call("get_state", target)) != actual or redone["revision"] == undone["revision"]:
        raise ProbeError("redo did not restore the committed state")
    if call("get_state", other) != b_before:
        raise ProbeError("history changed the other processor")

def validate_preview_wav(result, uri, frames):
    contents = result.get("contents")
    if not isinstance(contents, list) or len(contents) != 1:
        raise ProbeError("resource has no single actual WAV content")
    content = contents[0]
    if content.get("uri") != uri or content.get("mimeType") != "audio/wav" or not isinstance(content.get("blob"), str):
        raise ProbeError("resource URI/MIME/blob mismatch")
    try:
        wav = base64.b64decode(content["blob"], validate=True)
    except ValueError as error:
        raise ProbeError("invalid WAV base64") from error
    if frames < 1 or frames > 576000 or len(wav) != 44 + frames * 8:
        raise ProbeError("WAV frame count/size mismatch")
    header = struct.unpack("<4sI4s4sIHHIIHH4sI", wav[:44])
    if header != (b"RIFF", len(wav)-8, b"WAVE", b"fmt ", 16, 3, 2, 48000, 384000, 8, 32, b"data", frames*8):
        raise ProbeError("WAV header/rate/channels/IEEE-float mismatch")
    peak = 0.0
    squared = 0.0
    for (sample,) in struct.iter_unpack("<f", wav[44:]):
        if not math.isfinite(sample):
            raise ProbeError("non-finite WAV sample")
        peak = max(peak, abs(sample))
        squared += sample * sample
    return {"bytes": len(wav), "frames": frames, "float_samples": frames*2, "sample_rate": 48000,
            "channels": 2, "mimeType": "audio/wav", "sha256": hashlib.sha256(wav).hexdigest(),
            "peak": peak, "rms": math.sqrt(squared/(frames*2))}

def catalog_workflow(client, thread_id, evidence):
    ident=31000
    def call(tool,arguments):
        nonlocal ident
        ident+=1
        return tool_call(client,thread_id,ident,tool,arguments,evidence)
    rows=call("list_instances",{}).get("instances",[])
    if len(rows)!=2:
        raise ProbeError("catalog workflow requires two actual VST3 endpoints")
    target={"instance_id":rows[0]["instance_id"]}
    before=call("get_state",target)
    entries=[]
    arguments={**target,"kind":"racks","limit":65536}
    while True:
        page=call("get_catalog",arguments)
        if not isinstance(page.get("entries"),list):
            raise ProbeError("actual rack catalog has no entries")
        entries+=page["entries"]
        if page.get("cursor") is None:
            break
        arguments={**arguments,"snapshot":page["snapshot"],"cursor":page["cursor"]}
        if len(entries)>72:
            raise ProbeError("actual rack catalog exceeds its fixed bound")
    row=next((row for row in entries if row.get("name")=="Copper Arc" and row.get("role")=="synth"),None)
    if row is None:
        raise ProbeError("actual reference rack definition unavailable")
    call("get_catalog",{**target,"snapshot":page["snapshot"],"kind":"definition","design_id":row["id"],"design_revision":row["revision"],"content_sha256":row["content_sha256"]})
    edits={"parameter_edits":[{"id":"global.master_level","normalized":.35}],
           "macro_edits":[{"index":1,"target_id":"global.master_level","amount":.2}],
           "pattern":{"bars":4,"meter_numerator":4,"meter_denominator":4,"grid":"sixteenth","swing":0,
                      "notes":[{"start_beats":0,"duration_beats":.25,"pitch":64,"velocity":100,"channel":1}]}}
    combined=call("prepare_edit",{**target,"snapshot":before["snapshot"],"policy":{"scope":"both","allow_macro_changes":True},"edits":edits})
    call("get_edit",{**target,"edit_id":combined["edit_id"]})
    call("discard_edit",{**target,"edit_id":combined["edit_id"]})
    selection={"local_id":1,"design":row["id"],"revision":row["revision"],"content_sha256":row["content_sha256"],
               "destination":{"role":"synth","chain":4,"position":1}}
    library=call("prepare_edit",{**target,"snapshot":before["snapshot"],"policy":{},"edits":{"catalog_selections":[selection]}})
    call("get_edit",{**target,"edit_id":library["edit_id"]})
    call("discard_edit",{**target,"edit_id":library["edit_id"]})
    if call("get_state",target)!=before:
        raise ProbeError("catalog or staged composite/library edit changed committed sound")


def preview_workflow(client, thread_id, evidence, instance_index=0):
    ident = 21000
    def call(tool, arguments):
        nonlocal ident
        ident += 1
        return tool_call(client, thread_id, ident, tool, arguments, evidence["preview_calls"])
    listed = call("list_instances", {})["instances"]
    if len(listed) != 2:
        raise ProbeError("preview fixture requires two real processors")
    target = {"instance_id": listed[instance_index]["instance_id"]}
    other = {"instance_id": listed[1-instance_index]["instance_id"]}
    before = call("get_state", target)
    b_before = call("get_state", other)
    staged = call("prepare_edit", {**target, "snapshot": before["snapshot"], "policy": {},
                  "edits": {"parameter_edits": [{"id": "global.master_level", "normalized": .23}]}})
    edit = {**target, "edit_id": staged["edit_id"]}
    call("get_edit", edit)
    job = call("render_preview", edit)
    job_args = {**target, "job_id": job["job_id"]}
    deadline = time.monotonic() + 60
    while True:
        complete = call("get_job", job_args)
        if complete.get("job_status") == "ready":
            break
        if complete.get("job_status") not in ("queued", "running") or time.monotonic() >= deadline:
            raise ProbeError("actual preview render did not complete")
        time.sleep(.025)
    if complete.get("frames") != 576000 or complete.get("sample_rate") != 48000:
        raise ProbeError("fixture did not render the actual maximum preview")
    ident += 1
    client.send({"id": ident, "method": "mcpServer/resource/read", "params": {
        "server": "air_mcp_fixture", "threadId": thread_id, "uri": complete["resource_uri"]}})
    evidence["preview_resource"] = validate_preview_wav(client.receive(ident)["result"], complete["resource_uri"], complete["frames"])
    evidence.setdefault("preview_resources", []).append({"instance_id": target["instance_id"], **evidence["preview_resource"]})
    for key in ("peak", "rms"):
        if abs(evidence["preview_resource"][key] - complete["measurements"][key]) > 1e-12:
            raise ProbeError("WAV measurements differ from actual native render")
    call("play_preview", job_args)
    call("stop_preview", job_args)
    call("cancel_job", job_args)
    call("discard_edit", edit)
    if call("get_state", target) != before or call("get_state", other) != b_before:
        raise ProbeError("preview/audition changed committed state or the other instance")


@contextmanager
def native_edit_fixture(args, evidence):
    if args.mode == "workflow":
        if args.processors is None:
            raise ProbeError("workflow mode requires the actual VST3 host fixture via --processors")
        receipt = args.root / "workflow-ready.json"
        if receipt.exists() or (args.root / "workflow.stop").exists():
            raise ProbeError("refusing an existing actual-VST3 fixture receipt or stop signal")
        command = [str(args.processors), "--hold", "--output", str(args.root)]
        environment = os.environ.copy()
        environment["AIRACK_MCP_FIXTURE_ROOT"] = str(args.root)
        environment["AIRACK_MCP_PIPE_SUFFIX"] = args.suffix
        evidence["native_commands"] = [command]
        with (args.root / "actual-vst3-host.log").open("wb") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=environment)
            try:
                deadline = time.monotonic() + 30
                while True:
                    if process.poll() is not None:
                        raise ProbeError("actual VST3 host exited during startup")
                    try:
                        ready = json.loads(receipt.read_text(encoding="utf-8-sig"))
                    except (OSError, ValueError):
                        ready = None
                    if ready is not None:
                        validate_workflow_ready(ready, args)
                        evidence["actual_vst3_fixture"] = ready
                        break
                    if time.monotonic() >= deadline:
                        raise ProbeError("actual VST3 host did not become ready")
                    time.sleep(.02)
                yield
            finally:
                (args.root / "workflow.stop").write_text("owned probe stop", encoding="utf-8")
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=3)
                    evidence["actual_vst3_forced_retirement"] = True
                evidence["actual_vst3_host_exit"] = process.returncode
                if process.returncode != 0 or evidence.get("actual_vst3_forced_retirement"):
                    raise ProbeError("actual VST3 host did not retire cleanly")
        return
    if args.mode not in ("edit", "preview"):
        yield
        return
    if args.processors is None:
        raise ProbeError("edit mode requires --processors native fixture executable")
    children = []
    logs = []
    try:
        commands = [[str(args.companion), "--mcp-service", "--fixture-root", str(args.root), "--pipe-suffix", args.suffix]]
        commands += [[str(args.processors), "--processor", str(args.root), args.suffix, label] + (["preview"] if args.mode == "preview" else []) for label in ("codex-a", "codex-b")]
        evidence["native_commands"] = commands
        for index, command in enumerate(commands):
            logs.append((args.root / f"native-child-{index}.log").open("wb"))
            children.append(subprocess.Popen(command, stdout=logs[-1], stderr=subprocess.STDOUT))
        deadline = time.monotonic() + 15
        for label in ("codex-a", "codex-b"):
            while True:
                if any(p.poll() is not None for p in children):
                    raise ProbeError("native edit fixture exited during startup")
                try:
                    observation = json.loads((args.root / (label + ".json")).read_text(encoding="utf-8-sig"))
                except (OSError, ValueError):
                    observation = {}
                if observation.get("connected") and observation.get("editor_closed"):
                    evidence[label] = observation
                    break
                if time.monotonic() >= deadline:
                    raise ProbeError("native edit fixture did not connect")
                time.sleep(.02)
        yield
    finally:
        for label in ("codex-a", "codex-b"):
            (args.root / (label + ".stop")).write_text("stop", encoding="utf8")
        forced = False
        for process in children[1:]:
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                forced = True
                process.terminate()
                process.wait(timeout=3)
        if children:
            children[0].terminate()
            children[0].wait(timeout=3)
        for log in logs:
            log.close()
        evidence["native_processor_exits"] = [p.returncode for p in children[1:]]
        evidence["native_service_terminated_by_fixture"] = bool(children)
        if forced or any(p.returncode != 0 for p in children[1:]):
            raise ProbeError("native processors did not retire cleanly")

def absolute(value):
    path = Path(value)
    if not path.is_absolute():
        raise argparse.ArgumentTypeError("absolute path required")
    return path

def validate_workflow_ready(ready, args):
    if not isinstance(ready, dict) or ready.get("ready") is not True or ready.get("scope") != "actual JUCE-hosted VST3; public host APIs only":
        raise ProbeError("missing actual-VST3 ready prerequisite")
    if ready.get("suffix") != args.suffix or Path(ready.get("root", "")).resolve() != args.root.resolve() or ready.get("editor_closed") is not True:
        raise ProbeError("actual-VST3 fixture namespace/lifetime mismatch")
    rows = ready.get("instances")
    if not isinstance(rows, list) or len(rows) != 2:
        raise ProbeError("actual-VST3 fixture requires two discovered endpoints")
    try:
        ids = [uuid.UUID(row["instance_id"]) for row in rows]
    except (ValueError, TypeError, KeyError, AttributeError) as error:
        raise ProbeError("actual-VST3 fixture has invalid discovered identities") from error
    if len(set(ids)) != 2 or any(not value.int for value in ids):
        raise ProbeError("actual-VST3 fixture identities are not distinct live addresses")
    if Path(ready.get("helper", "")).resolve() != args.companion.resolve():
        raise ProbeError("actual-VST3 fixture helper differs from the probed helper")
    binary = Path(ready.get("plugin_binary", ""))
    if not binary.is_file() or hashlib.sha256(binary.read_bytes()).hexdigest() != ready.get("plugin_sha256"):
        raise ProbeError("actual-VST3 fixture product identity mismatch")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("codex", "companion", "root", "output"):
        parser.add_argument("--" + name, type=absolute, required=True)
    parser.add_argument("--suffix", required=True)
    parser.add_argument("--mode", choices=("discovery", "edit", "preview", "workflow"), default="discovery")
    parser.add_argument("--processors", type=absolute)
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    evidence = {"mode": args.mode, "success": False, "inference_started": False,
                "companion_sha256": hashlib.sha256(args.companion.read_bytes()).hexdigest(),
                "codex_sha256": hashlib.sha256(args.codex.read_bytes()).hexdigest()}
    if args.processors:
        evidence["processor_fixture_sha256"] = hashlib.sha256(args.processors.read_bytes()).hexdigest()
    try:
        with native_edit_fixture(args, evidence), tempfile.TemporaryDirectory(prefix="codex-probe-", dir=args.root) as directory:
            probe_home = Path(directory)
            environment = os.environ.copy()
            # Child-only configuration isolation: never copy auth or modify the caller's environment.
            environment["CODEX_HOME"] = str(probe_home)
            server = {"command": str(args.companion.resolve()), "args": ["--mcp-stdio", "--fixture-root", str(args.root.resolve()),
                       "--pipe-suffix", args.suffix], "required": True}
            command = [str(args.codex), "app-server", "--stdio", "-c", "mcp_servers={}",
                       "-c", "features.plugins=false", "-c", "features.remote_plugin=false",
                       "-c", "features.plugin_sharing=false", "-c", "features.apps=false"]
            evidence["app_server_command"] = command
            thread_id = None
            with args.output.with_suffix(".stderr.log").open("wb") as diagnostics:
                with ChildTransport(command, env=environment, cwd=probe_home, stderr=diagnostics) as client:
                    client.send({"id": 1, "method": "initialize", "params": {"clientInfo": {"name": "air-mcp-probe", "version": "1"}}})
                    evidence["initialize"] = client.receive(1)["result"]
                    client.send({"method": "initialized", "params": {}})
                    client.send({"id": 2, "method": "thread/start", "params": {"ephemeral": True, "cwd": str(probe_home),
                                 "config": {"mcp_servers": {"air_mcp_fixture": server}}}})
                    thread_id = client.receive(2)["result"]["thread"]["id"]
                    evidence["thread_id"] = thread_id
                    try:
                        evidence["inventory"] = wait_inventory(client, thread_id, 30)
                        client.send({"id": 10000, "method": "mcpServerStatus/list", "params": {"threadId": thread_id, "detail": "full"}})
                        evidence["configured_servers"] = [s["name"] for s in client.receive(10000)["result"]["data"]]
                        if args.mode == "edit":
                            evidence["edit_calls"] = []
                            edit_workflow(client, thread_id, evidence["edit_calls"])
                        if args.mode == "preview":
                            evidence["preview_calls"] = []
                            preview_workflow(client, thread_id, evidence)
                        if args.mode == "workflow":
                            evidence["catalog_calls"] = []
                            catalog_workflow(client, thread_id, evidence["catalog_calls"])
                            evidence["edit_calls"] = []
                            edit_workflow(client, thread_id, evidence["edit_calls"])
                            evidence["preview_calls"] = []
                            preview_workflow(client, thread_id, evidence)
                            preview_workflow(client, thread_id, evidence, 1)
                        if evidence["configured_servers"] != ["air_mcp_fixture"]:
                            raise ProbeError("probe inherited unrelated MCP servers")
                    finally:
                        client.send({"id": 10001, "method": "thread/unsubscribe", "params": {"threadId": thread_id}})
                        evidence["unsubscribe"] = client.receive(10001)["result"]
                    evidence["notifications"] = list(client.notifications)
                    evidence["sent_methods"] = client.sent_methods
                    if any(method and method.startswith("turn/") for method in client.sent_methods):
                        raise ProbeError("probe unexpectedly started an inference turn")
                    process = client.process
                evidence["app_server_exit"] = process.returncode
            evidence["isolated_config"] = True
        evidence["isolated_home_removed"] = not probe_home.exists()
        evidence["success"] = evidence["app_server_exit"] == 0
        if not evidence["success"]:
            raise ProbeError("app-server did not exit cleanly")
    except Exception as error:
        evidence["error"] = str(error)
    args.output.write_text(json.dumps(evidence, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("Codex MCP " + args.mode + " " + ("PASS" if evidence["success"] else "FAIL") + ": " + str(args.output))
    return 0 if evidence["success"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
