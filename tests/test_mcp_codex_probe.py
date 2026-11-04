"""Exercise correlation and failure detection with an owned fake app-server."""
import json
import base64
import struct
from pathlib import Path
import sys
import time
import unittest
import tempfile
import hashlib
from contextlib import contextmanager
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from mcp_codex_probe import ChildTransport, ProbeError, wait_inventory, tool_call, edit_workflow, validate_preview_wav, validate_workflow_ready

def fake(mode):
    if mode == "delayed":
        time.sleep(1)
    for line in sys.stdin:
        request = json.loads(line)
        if "id" not in request:
            continue
        ident = request["id"]
        if request["method"] == "fixture/ready":
            print(json.dumps({"id": ident, "result": {"ready": True}}), flush=True)
            continue
        if mode == "timeout":
            time.sleep(2)
            continue
        if mode in ("wrong", "interleaved"):
            print(json.dumps({"id": 999, "result": {"wrong": True}}), flush=True)
            print(json.dumps({"method": "startup", "params": {"status": "ready"}}), flush=True)
            if mode == "wrong":
                continue
        if request["method"] == "mcpServerStatus/list":
            status = {"name": "air_mcp_fixture", "runtimeStatus": "connected", "tools": {}, "serverInfo": {"name": "AIR", "version": "1"}, "serverCapabilities": {"tools": {}}}
            if mode == "ready":
                status["tools"] = {name: {"name": name, "inputSchema": {"type": "object"}} for name in ("list_instances", "get_state")}
            if mode == "failed":
                status["runtimeStatus"] = "failed"
            result = {"data": [status]}
        else:
            result = {"correct": True}
        print(json.dumps({"id": ident, "result": result}), flush=True)

class ProbeTests(unittest.TestCase):
    def test_workflow_receipt_requires_actual_product_identity_and_distinct_discovery(self):
        # Receipt parser unit data only; this does not establish product acceptance.
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            module=root / "product with spaces.vst3"
            module.write_bytes(b"receipt parser fixture")
            args=SimpleNamespace(root=root,suffix="owned-fixture",companion=root / "helper.exe")
            ready={"ready": True,"scope": "actual JUCE-hosted VST3; public host APIs only", "suffix": args.suffix,
                   "root": str(root), "editor_closed": True, "helper": str(args.companion), "plugin_binary": str(module),
                   "plugin_sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
                   "instances": [{"instance_id": "25e61858-1b56-47c9-8615-9e7fb0bcf768"}, {"instance_id": "828047f6-edaf-4ea2-bb54-6d1baa624b90"}]}
            validate_workflow_ready(ready,args)
            for delta in ({"ready":False},{"scope":"native processor mock"},{"suffix":"foreign"},{"editor_closed":False},
                          {"plugin_sha256":"bad"},{"helper":str(root / "other.exe")},{"instances":[ready["instances"][0]]*2}):
                with self.assertRaises(ProbeError):
                    validate_workflow_ready({**ready,**delta},args)

    @contextmanager
    def child(self, mode):
        with ChildTransport([sys.executable, str(Path(__file__).resolve()), "--fake", mode]) as client:
            client.send({"id": 0, "method": "fixture/ready"})
            self.assertEqual(client.receive(0), {"id": 0, "result": {"ready": True}})
            yield client

    def test_preserves_interleaved_notifications_and_correlates_ids(self):
        with self.child("interleaved") as client:
            client.send({"id": 1, "method": "initialize"})
            self.assertTrue(client.receive(1, .5)["result"]["correct"])
            self.assertEqual(client.notifications[0]["method"], "startup")
            self.assertEqual(client.receive(999, .5)["result"], {"wrong": True})

    def test_delayed_fake_startup_keeps_response_budget(self):
        with self.child("delayed") as client:
            client.send({"id": 1, "method": "initialize"})
            self.assertTrue(client.receive(1, .5)["result"]["correct"])

    def test_wrong_id_cannot_satisfy_receive(self):
        with self.child("wrong") as client:
            client.send({"id": 1, "method": "initialize"})
            with self.assertRaisesRegex(ProbeError, "^timeout waiting for response 1$"):
                client.receive(1, .2)
            self.assertEqual(client.responses[999]["result"], {"wrong": True})

    def test_timeout_rejected_and_owned_child_retired(self):
        with self.child("timeout") as client:
            process = client.process
            client.send({"id": 1, "method": "initialize"})
            with self.assertRaisesRegex(ProbeError, "^timeout waiting for response 1$"):
                client.receive(1, .1)
        self.assertIsNotNone(process.poll())

    def test_false_ready_without_tools_is_rejected(self):
        with self.child("empty") as client:
            with self.assertRaisesRegex(ProbeError, "^connected fixture has no required tool inventory$"):
                wait_inventory(client, "thread", .5)

    def test_startup_failure_is_rejected(self):
        with self.child("failed") as client:
            with self.assertRaisesRegex(ProbeError, "^fixture MCP startup failed$"):
                wait_inventory(client, "thread", .5)

    def test_standard_tool_result_rejects_error_or_missing_structured_content(self):
        class Client:
            def send(self, value): pass
            def receive(self, ident): return {"result": self.result}
        client = Client()
        for result in ({"isError": True, "structuredContent": {"status": "ready"}},
                       {"content": [{"type": "text", "text": "ready"}]},
                       {"structuredContent": {"status": "stale"}}):
            client.result = result
            with self.assertRaises(ProbeError):
                tool_call(client, "thread", 1, "apply_edit", {}, [])

    def test_edit_workflow_checks_real_structured_sequence_and_target_isolation(self):
        # This is a probe unit fake only; acceptance runs the installed app-server and native processors.
        class Client:
            def __init__(self, wrong=False): self.value=.5; self.revision=0; self.wrong=wrong; self.request=None
            def send(self, value): self.request=value
            def receive(self, ident):
                args=self.request["params"]; tool=args["tool"]; target=args["arguments"].get("instance_id")
                if tool == "list_instances": data={"instances": [{"instance_id": "a"}, {"instance_id": "b"}]}
                elif tool == "get_state": data={"snapshot": "snapshot", "revision": str(self.revision if target == "a" else 0),
                    "values": [{"id": "global.master_level", "normalized": self.value if target == "a" else (.3 if self.wrong and self.revision else .5)}]}
                elif tool == "get_catalog": data={"edits_schema": {}}
                elif tool in ("prepare_edit", "get_edit"): data={"edit_id": "edit", "diff": {"parameters": [1]}}
                elif tool == "apply_edit":
                    if self.revision == 0: self.value=.23; self.revision=1
                    data={"changed": True, "transaction_id": "1", "revision": "1"}
                elif tool == "undo_edit": self.value=.5; self.revision+=1; data={"revision": str(self.revision)}
                elif tool == "redo_edit": self.value=.23; self.revision+=1; data={"revision": str(self.revision)}
                else: raise AssertionError(tool)
                return {"id": ident, "result": {"structuredContent": {"status": "ready", **data}, "isError": False}}
        evidence=[]
        edit_workflow(Client(), "thread", evidence)
        self.assertEqual([row["tool"] for row in evidence].count("apply_edit"), 2)
        self.assertIn("redo_edit", [row["tool"] for row in evidence])
        with self.assertRaisesRegex(ProbeError, "other processor"):
            edit_workflow(Client(wrong=True), "thread", [])

    def test_resource_wav_requires_actual_float_header_size_and_samples(self):
        uri = "air-preview://fixture/epoch/resource"
        wav = struct.pack("<4sI4s4sIHHIIHH4sIff", b"RIFF",44,b"WAVE",b"fmt ",16,3,2,48000,384000,8,32,b"data",8,.25,-.5)
        def content(data): return {"contents": [{"uri": uri, "mimeType": "audio/wav", "blob": base64.b64encode(data).decode()}]}
        self.assertEqual(validate_preview_wav(content(wav), uri, 1)["float_samples"], 2)
        for invalid in (wav[:-1], wav[:20]+b"\x01\x00"+wav[22:], wav[:-4]+struct.pack("<f", float("nan"))):
            with self.assertRaises(ProbeError): validate_preview_wav(content(invalid), uri, 1)
        with self.assertRaises(ProbeError): validate_preview_wav({"contents": [{"uri": uri, "text": "done"}]}, uri, 1)

    def test_actual_tool_schema_required(self):
        with self.child("ready") as client:
            status = wait_inventory(client, "thread", .5)
            self.assertEqual(status["name"], "air_mcp_fixture")
            self.assertEqual(set(status["tools"]), {"list_instances", "get_state"})

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fake":
        fake(sys.argv[2])
    else:
        unittest.main()
