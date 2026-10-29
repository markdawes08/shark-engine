"""Real terminal-host export failure controls; these helpers are not packages."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest

import test_authoring_protocol_process as pipes


HELPER = r'''
import json
import os
from pathlib import Path
import sys

assert sys.argv[1:3] == ["authoring-export", "--request"] and len(sys.argv) == 4
config = json.loads(Path(__file__).with_suffix(".json").read_text(encoding="utf-8"))
request_file = Path(sys.argv[3])
request = json.loads(request_file.read_text(encoding="utf-8"))
trace = {"pid": os.getpid(), "phases": ["started"]}
def phase(name):
    trace["phases"].append(name)
    Path(config["trace"]).write_text(json.dumps(trace), encoding="utf-8")
def emit(value):
    sys.stdout.buffer.write(json.dumps(value).encode("utf-8") + b"\n")
    sys.stdout.buffer.flush()
def receive(expected):
    assert sys.stdin.buffer.readline(17) == expected + b"\n"
def report(published, status):
    return {"format": "shark.client-export", "schema_version": 1,
            "status": status, "published": published, "qualification": "NOT_RUN",
            "renderer_initialized": False, "platform": "windows-x86_64", "artifacts": []}

phase("configured")
if config["mode"] == "before":
    result = report(False, "ERROR")
    result["error"] = {"code": "execution_failed", "message": "Controlled preparation rejection"}
    emit(result)
    raise SystemExit(2)
emit({"format": "shark.authoring-export-helper", "schema_version": 1,
      "status": "INPUTS", "vertex": config["vertex"], "pixel": config["pixel"]})
receive(b"ASSEMBLE")
content = json.loads(Path(request["content_record"]).read_text(encoding="utf-8"))
assert content["dirty"] is True
stage = request_file.parent / ".runtime-package-control"
stage.mkdir()
(stage / "incomplete.txt").write_text("private incomplete stage", encoding="utf-8")
if config["mode"] == "stale":
    source = Path(request["project_root"]) / "scenes/FeatureWorld.sharkscene"
    with source.open("ab") as stream:
        stream.write(b"\n")
phase("ready")
emit({"format": "shark.authoring-export-helper", "schema_version": 1,
      "status": "READY_TO_PUBLISH"})
receive(b"PUBLISH")
phase("granted")
output = Path(request["output"])
output.mkdir()
(output / "failure-control.txt").write_text(config["mode"], encoding="utf-8")
phase("published-marker")
if config["mode"] == "lost":
    raise SystemExit(0)
assert config["mode"] == "after"
emit(report(True, "PASS"))
sys.stderr.write("Controlled failure after the publication report\n")
sys.stderr.flush()
raise SystemExit(7)
'''


class AuthoringExportControls(unittest.TestCase):
    def setUp(self):
        output = pipes.ROOT / "out"
        output.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="terminal-export-controls-", dir=output)
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.project = self.base / "caf\u00e9 project"
        (self.project / "scenes").mkdir(parents=True)
        self.original = {}
        for name in ("FeatureWorld.sharkproject", "scenes/FeatureWorld.sharkscene"):
            self.original[name] = (pipes.ARGS.project.parent / name).read_bytes()
            (self.project / name).write_bytes(self.original[name])
        self.helper = self.base / "controlled_helper.py"
        self.helper.write_text(HELPER, encoding="utf-8")
        text = (pipes.ROOT / "tests/gpu_shader_fixture.hpp").read_text(encoding="utf-8")
        self.shaders = {}
        for kind in ("vs", "ps"):
            body = re.search(r"scene_" + kind + r"_words\{(.*?)\};", text, re.S).group(1)
            words = [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]+)U", body)]
            path = self.base / (kind + ".spv")
            path.write_bytes(struct.pack(f"<{len(words)}I", *words))
            self.shaders[kind] = str(path)
        self.output = self.base / "published"
        self.trace = self.base / "trace.json"

    def assert_child_stopped(self, pid):
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        api.OpenProcess.restype = wintypes.HANDLE
        api.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        api.WaitForSingleObject.restype = wintypes.DWORD
        api.CloseHandle.argtypes = (wintypes.HANDLE,)
        api.CloseHandle.restype = wintypes.BOOL
        handle = api.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE on this known child.
        if not handle:
            self.assertEqual(ctypes.get_last_error(), 87)  # Process already reaped.
            return
        try:
            self.assertEqual(api.WaitForSingleObject(handle, 0), 0, "Export helper survived completion")
        finally:
            api.CloseHandle(handle)

    def exercise(self, mode):
        self.helper.with_suffix(".json").write_text(json.dumps({
            "mode": mode, "vertex": self.shaders["vs"], "pixel": self.shaders["ps"],
            "trace": str(self.trace)}), encoding="utf-8")
        options = ("--export-python", sys.executable, "--export-tool", str(self.helper))
        with pipes.PipeClient(self.base, self.project, "FeatureWorld.sharkproject",
                              options=options, timeout=10) as client:
            discovery = client.result("protocol.discover")
            self.assertTrue(discovery["capabilities"]["standalone_export"])
            state = client.result("scene.mutation_status")
            edited = client.result("scene.mutate", {
                "mutation_id": state["next"], "expected_revision": state["revision"],
                "operation": {"type": "command", "command": {"type": "set_name",
                    "entity_id": pipes.ENTITIES[0], "name": "Unsaved export control"}}})
            self.assertTrue(edited["outcome"]["ok"])
            self.assertTrue(edited["outcome"]["value"]["result"]["ok"])
            before = client.result("project.status")
            history = client.result("scene.history")
            mutation = client.result("scene.mutation_status")
            self.assertTrue(before["dirty"])
            self.assertEqual(history["undo_entries"], 1)
            reply = client.call("scene.export", {
                "expected_revision": before["revision"],
                "expected_catalog": client.result("asset.catalog.status")["revision"],
                "expected_cache": client.result("asset.cache.status")["revision"],
                "camera_id": pipes.ENTITIES[0], "target": {"id": "control", "side": "client"},
                "build_dir": str(self.base), "crt_dir": str(self.base),
                "shaders": str(self.base), "output": str(self.output)})
            self.assertEqual(client.result("project.status"), before)
            self.assertEqual(client.result("scene.history"), history)
            self.assertEqual(client.result("scene.mutation_status"), mutation)
            trace = json.loads(self.trace.read_text(encoding="utf-8"))
            self.assert_child_stopped(trace["pid"])
            self.assertEqual(list(self.base.glob(".runtime-export-*")), [])
            self.assertEqual(list(self.base.glob(".runtime-package-*")), [])
            for name, expected in self.original.items():
                if mode == "stale" and name.endswith(".sharkscene"):
                    expected += b"\n"
                self.assertEqual((self.project / name).read_bytes(), expected)
            client.finish()
        return reply, trace

    def test_helper_rejection_before_publication_is_false_and_preserves_owner(self):
        reply, trace = self.exercise("before")
        self.assertEqual(reply["result"]["status"], "ERROR")
        self.assertFalse(reply["result"]["published"])
        self.assertEqual(reply["result"]["error"]["message"], "Controlled preparation rejection")
        self.assertEqual(trace["phases"], ["started", "configured"])
        self.assertFalse(self.output.exists())

    def test_source_change_at_ready_rejects_final_fence_without_publication(self):
        reply, trace = self.exercise("stale")
        self.assertEqual(reply["error"]["data"]["code"], "conflict", reply)
        self.assertEqual(trace["phases"], ["started", "configured", "ready"])
        self.assertFalse(self.output.exists())

    def test_reported_publication_survives_later_helper_exit_failure(self):
        reply, trace = self.exercise("after")
        self.assertEqual(reply["result"]["status"], "FAIL", reply)
        self.assertTrue(reply["result"]["published"])
        self.assertEqual(reply["result"]["error"]["code"], "execution_failed")
        self.assertEqual(trace["phases"][-2:], ["granted", "published-marker"])
        self.assertEqual((self.output / "failure-control.txt").read_text(encoding="utf-8"), "after")

    def test_lost_completion_after_grant_requires_publication_reconciliation(self):
        reply, trace = self.exercise("lost")
        self.assertEqual(reply["error"]["data"]["code"], "execution_failed", reply)
        self.assertIn("publication outcome is unknown", reply["error"]["message"])
        context = reply["error"]["data"]["context"]
        self.assertEqual(context["operation"], "authoring.export")
        self.assertEqual(Path(context["path"]), self.output)
        self.assertEqual(trace["phases"][-2:], ["granted", "published-marker"])
        self.assertEqual((self.output / "failure-control.txt").read_text(encoding="utf-8"), "lost")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", type=Path, required=True)
    parser.add_argument("--project", type=Path,
                        default=pipes.ROOT / "assets/FeatureWorld/FeatureWorld.sharkproject")
    args, remaining = parser.parse_known_args()
    if os.name != "nt":
        raise SystemExit("Native export controls require Windows; this is not a passing skipped selection")
    args.author, args.project = args.author.resolve(), args.project.resolve()
    pipes.ARGS = args
    unittest.main(argv=[sys.argv[0], *remaining])
