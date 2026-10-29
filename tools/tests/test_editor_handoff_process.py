"""Connected CPU editor adapters and public terminal processes, not native UI acceptance."""
from __future__ import annotations

import argparse
import copy
from dataclasses import asdict
import json
import math
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

import test_authoring_protocol_process as pipes
import test_authoring_workflow_process as workflow


FIELDS = ["name", "transform", "hierarchy", "camera", "mesh_renderer"]
DURABILITY = {"file_flushed", "file_and_directory_flushed", "directory_flush_failed"}
EDITOR_ROW_BYTES = 65536
IDS = workflow.IDS
TRANSFORM_PARENT = "17d00000-0000-4000-8000-000000000001"
QUARTER_TURN = [0, 0, math.sqrt(.5), math.sqrt(.5)]
IDENTITY = [0, 0, 0, 1]
ARGS = None


class EditorPhase:
    """One fixed-action CPU fixture and its single save/close barrier."""

    def __init__(self, root, transcript, phase="edit"):
        self.transcript = transcript
        self.stderr = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [str(ARGS.editor_handoff), "--root", str(root), "--phase", phase],
            cwd=root.parent, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr)
        self.rows = queue.Queue(maxsize=3)
        self.reader_failure = threading.Event()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while True:
                line = self.process.stdout.readline(EDITOR_ROW_BYTES + 3)
                self.rows.put_nowait(line)
                if not line or len(line) > EDITOR_ROW_BYTES + 2:
                    return
        except (OSError, ValueError, queue.Full):
            self.reader_failure.set()

    def row(self):
        try:
            line = self.rows.get(timeout=20)
        except queue.Empty as failure:
            raise AssertionError("Editor handoff fixture response deadline expired") from failure
        assert not self.reader_failure.is_set(), "Editor fixture output exceeded its bound"
        assert line and line.endswith(b"\n") and len(line.rstrip(b"\r\n")) <= EDITOR_ROW_BYTES, line[:1024]
        value = json.loads(line.decode("utf-8"), object_pairs_hook=pipes.unique)
        self.transcript.append(value)
        return value

    def save_close(self):
        self.process.stdin.write(b"save-close\n")
        self.process.stdin.flush()
        self.process.stdin.close()
        row = self.row()
        assert self.process.wait(timeout=10) == 0, self.process.returncode
        self.reader.join(timeout=1)
        assert not self.reader.is_alive() and not self.reader_failure.is_set()
        assert self.rows.get(timeout=1) == b"", "Unexpected trailing fixture output"
        assert self.rows.empty()
        self.stderr.seek(0)
        assert self.stderr.read(4097) == b"", "Editor fixture wrote diagnostics to stderr"
        return row

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=2)
        self.reader.join(timeout=2)
        assert not self.reader.is_alive(), "Editor fixture reader survived child termination"
        self.stderr.seek(0)
        diagnostics = self.stderr.read(4097).decode("utf-8", errors="replace")
        if diagnostics:
            self.transcript.append({"editor_stderr": diagnostics})
        for stream in (self.process.stdin, self.process.stdout, self.stderr):
            if not stream.closed:
                stream.close()


class EditorHandoffProcessTests(unittest.TestCase):
    def completed(self, command, scratch, timeout=20):
        result = pipes.run_process(command, cwd=scratch, timeout=timeout)
        self.assertEqual(result.returncode, 0, asdict(result))
        self.assertFalse(result.timed_out, asdict(result))
        self.assertFalse(result.cleanup_errors, asdict(result))
        self.assertEqual(result.stderr, "", asdict(result))
        return result.stdout

    def probe(self, root, phase, transcript):
        output = self.completed([str(ARGS.editor_handoff), "--root", str(root), "--phase", phase],
                                root.parent)
        lines = output.splitlines()
        self.assertLessEqual(len(lines), 2)
        for line in lines:
            self.assertLessEqual(len(line.encode("utf-8")), EDITOR_ROW_BYTES)
        rows = [json.loads(line, object_pairs_hook=pipes.unique) for line in lines]
        transcript.extend(rows)
        return rows

    def entities(self, rows, name, translation, parent):
        self.assertEqual(len(rows), 2, rows)
        self.assertEqual({row["entity_id"] for row in rows}, {IDS["camera"], IDS["entity"]})
        for row in rows:
            camera = row["entity_id"] == IDS["camera"]
            self.assertEqual(row["name"], "Terminal camera" if camera else name)
            self.assertEqual(row["transform"]["local"], {
                "translation": [0, 0, 3] if camera else translation,
                "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] if camera else [2, 2, 2]})
            self.assertEqual(row["hierarchy"]["parent_id"], None if camera else parent)
            self.assertEqual(row["camera"], {**workflow.CAMERA, "orthographic_height": 4} if camera else None)
            self.assertEqual(row["mesh_renderer"], None if camera else {
                "asset_id": IDS["mesh"], "mesh_index": 0, "visible": True})

    def saved_entities(self, root, name, translation, parent):
        document = workflow.read_json(root / workflow.SCENE_PATH)
        self.assertEqual((document["format"], document["schema_version"],
                          document["project_id"], document["scene_id"]),
                         ("shark.scene", 1, IDS["project"], IDS["scene"]))
        self.assertEqual(len(document["entities"]), 2)
        rows = []
        for entity in document["entities"]:
            components = copy.deepcopy(entity["components"])
            for component in components.values():
                self.assertEqual(component.pop("schema_version"), 1)
            camera = entity["entity_id"] == IDS["camera"]
            self.assertEqual(set(components), {"shark.name", "shark.transform",
                                               "shark.camera" if camera else "shark.mesh_renderer"})
            rows.append({"entity_id": entity["entity_id"], "name": components["shark.name"]["value"],
                         "transform": {"local": components["shark.transform"]},
                         "hierarchy": {"parent_id": entity.get("parent")},
                         "camera": components.get("shark.camera"),
                         "mesh_renderer": components.get("shark.mesh_renderer")})
        self.entities(rows, name, translation, parent)

    def status(self, value, *, dirty):
        self.assertTrue(value["open"], value)
        self.assertIs(value["dirty"], dirty, value)
        self.assertEqual((value["project_id"], value["world_id"], value["scene_id"]),
                         (IDS["project"], IDS["world"], IDS["scene"]))

    def publication(self, value):
        self.assertTrue(value["published"], value)
        self.assertIn(value["durability"], DURABILITY)

    def dirty_guards(self, value):
        self.assertEqual(value, {"reject": "conflict", "cancelled": True, "replace": "conflict",
                                 "state_preserved": True, "history_preserved": True})

    def numbers(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        for observed, intended in zip(actual, expected):
            self.assertTrue(math.isfinite(observed), actual)
            self.assertAlmostEqual(observed, intended, delta=1e-9)

    def transform_local(self, actual, translation, rotation, scale):
        self.assertEqual(set(actual), {"translation", "rotation", "scale"})
        self.numbers(actual["translation"], translation)
        self.numbers(actual["rotation"], rotation)
        self.numbers(actual["scale"], scale)

    def transform_rows(self, rows, translation, rotation, scale):
        self.assertEqual({row["entity_id"] for row in rows},
                         {IDS["camera"], IDS["entity"], TRANSFORM_PARENT})
        self.assertEqual(len(rows), 3)
        for row in rows:
            identity = row["entity_id"]
            if identity == IDS["entity"]:
                self.assertEqual(row["name"], "Unsaved triangle \u96ea")
                self.assertEqual(row["hierarchy"]["parent_id"], TRANSFORM_PARENT)
                self.transform_local(row["transform"]["local"], translation, rotation, scale)
                self.assertIsNone(row["camera"])
                self.assertEqual(row["mesh_renderer"], workflow.MESH)
            else:
                parent = identity == TRANSFORM_PARENT
                self.assertEqual(row["name"], "Transform parent" if parent else "Terminal camera")
                self.assertIsNone(row["hierarchy"]["parent_id"])
                self.transform_local(row["transform"]["local"], [0, 0, 0] if parent else [0, 0, 3],
                                     QUARTER_TURN if parent else IDENTITY,
                                     [2, 2, 2] if parent else [1, 1, 1])
                self.assertEqual(row["camera"], None if parent else
                                 {**workflow.CAMERA, "orthographic_height": 16})
                self.assertIsNone(row["mesh_renderer"])

    def transform_saved(self, root, translation, rotation, scale):
        document = workflow.read_json(root / workflow.SCENE_PATH)
        self.assertEqual((document["format"], document["schema_version"],
                          document["project_id"], document["scene_id"]),
                         ("shark.scene", 1, IDS["project"], IDS["scene"]))
        rows = []
        for entity in document["entities"]:
            components = copy.deepcopy(entity["components"])
            for component in components.values():
                self.assertEqual(component.pop("schema_version"), 1)
            expected = {"shark.name", "shark.transform"}
            if entity["entity_id"] == IDS["entity"]:
                expected.add("shark.mesh_renderer")
            elif entity["entity_id"] == IDS["camera"]:
                expected.add("shark.camera")
            self.assertEqual(set(components), expected)
            rows.append({"entity_id": entity["entity_id"], "name": components["shark.name"]["value"],
                         "transform": {"local": components["shark.transform"]},
                         "hierarchy": {"parent_id": entity.get("parent")},
                         "camera": components.get("shark.camera"),
                         "mesh_renderer": components.get("shark.mesh_renderer")})
        self.transform_rows(rows, translation, rotation, scale)

    def transform_mutation(self, terminal, operation):
        state = terminal.result("scene.mutation_status")
        reply = terminal.result("scene.mutate", {"mutation_id": state["next"],
                                  "expected_revision": state["revision"], "operation": operation})
        self.assertTrue(reply["outcome"]["ok"], reply)
        self.assertTrue(reply["outcome"]["value"]["result"]["ok"], reply)
        return reply

    def test_transform_drag_round_trips_through_public_terminal(self):
        ARGS.scratch_parent.mkdir(parents=True, exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix="transforms-", dir=ARGS.scratch_parent))
        transcript = []
        try:
            self.connected_transforms(scratch, transcript)
        except BaseException:
            (scratch / "transform-handoff-failure.json").write_text(
                json.dumps(transcript, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print(f"Retained failed transform handoff inputs: {scratch}", file=sys.stderr)
            raise
        else:
            shutil.rmtree(scratch)

    def connected_transforms(self, scratch, transcript):
        output = scratch / "transform project"
        completion = json.loads(self.completed([
            sys.executable, str(pipes.ROOT / "examples/terminal_authoring.py"),
            "--author", str(ARGS.author), "--output", str(output)], scratch, timeout=40),
            object_pairs_hook=pipes.unique)
        self.assertEqual(completion["status"], "PASS", completion)
        workflow.check_workflow(workflow.read_json(output / "summary.json"), output)
        root = output / "project"
        preserved = {name: (root / name).read_bytes() for name in
                     (workflow.PROJECT_PATH, "models/triangle.gltf", "models/triangle.bin",
                      "licenses/fixture-MIT.txt", *workflow.SIDECARS)}
        with pipes.PipeClient(scratch, root, workflow.PROJECT_PATH) as terminal:
            commands = [
                {"type": "create_entity", "entity_id": TRANSFORM_PARENT, "name": "Transform parent",
                 "transform": {"translation": [0, 0, 0], "rotation": QUARTER_TURN, "scale": [2, 2, 2]}},
                {"type": "set_transform", "entity_id": IDS["entity"], "transform": {
                    "translation": [1, 0, 0], "rotation": IDENTITY, "scale": [1, 1, 1]}},
                {"type": "reparent_entity", "entity_id": IDS["entity"],
                 "parent_id": TRANSFORM_PARENT, "mode": "keep_local"},
                {"type": "set_camera", "entity_id": IDS["camera"],
                 "camera": {**workflow.CAMERA, "orthographic_height": 16}}]
            self.transform_mutation(terminal, {"type": "batch", "batch": {
                "id": "17d00000-0000-4000-8000-000000000002", "commands": commands}})
            saved = terminal.result("scene.save", {
                "expected_revision": terminal.result("project.status")["revision"]})
            self.assertTrue(saved["success"] and saved["current_scene_clean"], saved)
            self.publication(saved["publication"])
            terminal.result("project.close")
            terminal.finish()
        self.transform_saved(root, [1, 0, 0], IDENTITY, [1, 1, 1])

        # Column-major independent matrix oracles: parent Rz90*S2, then the
        # declared local TRS. These literals are never copied from a receipt.
        translated = [0, 2, 0, 0, -2, 0, 0, 0, 0, 0, 2, 0, 0, 6, 0, 1]
        final_world = [4, 0, 0, 0, 0, -3, 0, 0, 0, 0, 1, 0, -4, 6, 0, 1]
        with EditorPhase(root, transcript, "transforms") as editor:
            ready = editor.row()
            self.assertEqual((ready["event"], ready["seed"]), ("transforms-ready", 1337))
            self.status(ready["initial"]["status"], dirty=False)
            self.transform_rows(ready["initial"]["entities"], [1, 0, 0], IDENTITY, [1, 1, 1])
            self.assertEqual(ready["initial"]["history"]["undo_entries"], 0)
            self.assertEqual(ready["shear_rejection"], "invalid_argument")
            self.transform_local(ready["shear_parent"]["transform"]["local"],
                                 [0, 0, 0], QUARTER_TURN, [2, 1, 1])
            self.assertEqual(len(ready["previews"]), 2)
            for preview, y in zip(ready["previews"], (4, 6)):
                self.assertTrue(preview["marked"])
                self.numbers(preview["world"], [0, 2, 0, 0, -2, 0, 0, 0,
                                                0, 0, 2, 0, 0, y, 0, 1])
            expected_steps = [
                ("world_translate", [3, 0, 0], IDENTITY, [1, 1, 1], translated),
                ("undo", [1, 0, 0], IDENTITY, [1, 1, 1],
                 [0, 2, 0, 0, -2, 0, 0, 0, 0, 0, 2, 0, 0, 2, 0, 1]),
                ("redo", [3, 0, 0], IDENTITY, [1, 1, 1], translated),
                ("world_rotate", [3, 0, 0], QUARTER_TURN, [1, 1, 1],
                 [-2, 0, 0, 0, 0, -2, 0, 0, 0, 0, 2, 0, 0, 6, 0, 1]),
                ("local_translate", [3, 2, 0], QUARTER_TURN, [1, 1, 1],
                 [-2, 0, 0, 0, 0, -2, 0, 0, 0, 0, 2, 0, -4, 6, 0, 1]),
                ("signed_scale", [3, 2, 0], QUARTER_TURN, [-2, 1.5, .5], final_world)]
            self.assertEqual(len(ready["steps"]), len(expected_steps))
            for step, (name, position, rotation, scale, world) in zip(ready["steps"], expected_steps):
                self.assertEqual(step["step"], name)
                observed = step["entity"]
                self.assertEqual(observed["entity_id"], IDS["entity"])
                self.assertEqual(observed["hierarchy"]["parent_id"], TRANSFORM_PARENT)
                self.transform_local(observed["transform"]["local"], position, rotation, scale)
                self.assertEqual(observed["transform"]["world_matrix"]["layout"], "column_major")
                self.numbers(observed["transform"]["world_matrix"]["values"], world)
            self.status(ready["status"], dirty=True)
            self.transform_rows(ready["entities"], [3, 2, 0], QUARTER_TURN, [-2, 1.5, .5])
            self.assertEqual((ready["history"]["undo_entries"], ready["history"]["redo_entries"]), (4, 0))
            self.assertEqual(ready["history"]["next_undo"]["command_count"], 1)
            stale = ready["stale"]
            self.assertEqual(stale["state"], "recorded")
            self.assertTrue(stale["outcome"]["ok"])
            self.assertFalse(stale["outcome"]["value"]["result"]["ok"])
            self.assertEqual(stale["outcome"]["value"]["result"]["error"]["code"], "conflict")
            self.publication(ready["mid_save"])
            self.assertTrue(ready["mid_save"]["current_scene_clean"])
            self.dirty_guards(ready["guards"])
            self.transform_saved(root, [3, 0, 0], IDENTITY, [1, 1, 1])
            with pipes.PipeClient(scratch, root) as competitor:
                busy = competitor.call("project.open", {"path": workflow.PROJECT_PATH})
                self.assertEqual(busy["error"]["data"]["code"], "conflict", busy)
                competitor.finish()
            closed = editor.save_close()
            self.assertEqual(closed["event"], "closed")
            self.publication(closed["save"])
            self.assertEqual(closed["history_before"], ready["history"])
            self.assertEqual(closed["history_after"], ready["history"])
            self.assertFalse(closed["status"]["open"])
        self.transform_saved(root, [3, 2, 0], QUARTER_TURN, [-2, 1.5, .5])

        with pipes.PipeClient(scratch, root, workflow.PROJECT_PATH) as terminal:
            self.status(terminal.result("project.status"), dirty=False)
            self.assertEqual(terminal.result("scene.history")["undo_entries"], 0)
            self.assertNotEqual(terminal.result("scene.mutation_status")["scope"],
                                ready["mutation_status"]["scope"])
            def inspect():
                page = terminal.result("scene.inspect", {"entity_id": IDS["entity"], "fields": FIELDS})
                self.assertTrue(page["complete"])
                return page["entities"][0]
            self.numbers(inspect()["transform"]["world_matrix"]["values"], final_world)
            # Reproduce independently specified local values through public
            # terminal commands, after an explicit baseline reset on this owner.
            self.transform_mutation(terminal, {"type": "command", "command": {
                "type": "set_transform", "entity_id": IDS["entity"], "transform": {
                    "translation": [1, 0, 0], "rotation": IDENTITY, "scale": [1, 1, 1]}}})
            stale_revision = terminal.result("project.status")["revision"]
            self.transform_mutation(terminal, {"type": "command", "command": {
                "type": "set_transform", "entity_id": IDS["entity"], "transform": {
                    "translation": [3, 2, 0], "rotation": QUARTER_TURN, "scale": [-2, 1.5, .5]}}})
            self.numbers(inspect()["transform"]["world_matrix"]["values"], final_world)
            state = terminal.result("scene.mutation_status")
            stale = terminal.result("scene.mutate", {"mutation_id": state["next"],
                "expected_revision": stale_revision, "operation": {"type": "command", "command": {
                    "type": "set_transform", "entity_id": IDS["entity"], "transform": {
                        "translation": [99, 99, 0], "rotation": IDENTITY, "scale": [1, 1, 1]}}}})
            self.assertEqual(stale["state"], "recorded")
            self.assertTrue(stale["outcome"]["ok"], stale)
            self.assertFalse(stale["outcome"]["value"]["result"]["ok"])
            self.assertEqual(stale["outcome"]["value"]["result"]["error"]["code"], "conflict")
            self.numbers(inspect()["transform"]["world_matrix"]["values"], final_world)
            # Return an additional authored transform for the editor to continue.
            self.transform_mutation(terminal, {"type": "command", "command": {
                "type": "set_transform", "entity_id": IDS["entity"], "transform": {
                    "translation": [4, 2, 0], "rotation": QUARTER_TURN, "scale": [-2, 1.5, .5]}}})
            history = terminal.result("scene.history")
            saved = terminal.result("scene.save", {
                "expected_revision": terminal.result("project.status")["revision"]})
            self.assertTrue(saved["success"] and saved["current_scene_clean"], saved)
            self.publication(saved["publication"])
            self.assertEqual(terminal.result("scene.history"), history)
            terminal_scope = terminal.result("scene.mutation_status")["scope"]
            terminal.result("project.close")
            terminal.finish()
        self.transform_saved(root, [4, 2, 0], QUARTER_TURN, [-2, 1.5, .5])
        final_source = (root / workflow.SCENE_PATH).read_bytes()
        reopened, = self.probe(root, "transforms-continue", transcript)
        self.assertEqual(reopened["event"], "transforms-reopened")
        self.assertTrue(reopened["closed"])
        self.status(reopened["status"], dirty=False)
        self.assertEqual((reopened["history"]["undo_entries"], reopened["history"]["redo_entries"]), (0, 0))
        self.assertNotEqual(reopened["mutation_status"]["scope"], terminal_scope)
        self.transform_rows(reopened["entities"], [4, 2, 0], QUARTER_TURN, [-2, 1.5, .5])
        self.numbers(reopened["world"], [4, 0, 0, 0, 0, -3, 0, 0, 0, 0, 1, 0, -4, 8, 0, 1])
        self.assertEqual((root / workflow.SCENE_PATH).read_bytes(), final_source)
        for path, contents in preserved.items():
            self.assertEqual((root / path).read_bytes(), contents, path)

    def test_saved_project_moves_between_editor_adapters_and_terminal(self):
        # Successful projects are disposable. An unexpected failure retains its
        # exact inputs and compact phase transcript at the printed scratch path.
        ARGS.scratch_parent.mkdir(parents=True, exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix="scenario-", dir=ARGS.scratch_parent))
        transcript = []
        try:
            self.connected(scratch, transcript)
        except BaseException:
            (scratch / "handoff-failure.json").write_text(
                json.dumps(transcript, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print(f"Retained failed editor handoff inputs: {scratch}", file=sys.stderr)
            raise
        else:
            shutil.rmtree(scratch)

    def connected(self, scratch, transcript):
        output = scratch / "caf\u00e9 \u4e16\u754c"
        completion = json.loads(self.completed([
            sys.executable, str(pipes.ROOT / "examples/terminal_authoring.py"),
            "--author", str(ARGS.author), "--output", str(output)], scratch, timeout=40),
            object_pairs_hook=pipes.unique)
        self.assertEqual(completion, {"status": "PASS", "summary": str(output / "summary.json"), "errors": []})
        baseline = workflow.read_json(output / "summary.json")
        workflow.check_workflow(baseline, output)
        root = output / "project"
        original_scene = (root / workflow.SCENE_PATH).read_bytes()
        original_project = (root / workflow.PROJECT_PATH).read_bytes()
        preserved = {path: (root / path).read_bytes() for path in
                     ("models/triangle.gltf", "models/triangle.bin", "licenses/fixture-MIT.txt",
                      workflow.SIDECARS[1])}
        metadata = workflow.read_json(root / workflow.SIDECARS[0])
        metadata["origin"] = "Editor-authored continuation"
        del metadata["settings"]["scene"]
        prior_import = baseline["imports"]["modified"]["descriptor"]

        with EditorPhase(root, transcript) as editor:
            ready = editor.row()
            self.assertEqual((ready["event"], ready["seed"]), ("ready", 1337))
            self.status(ready["initial"]["status"], dirty=False)
            workflow.check_entities(ready["initial"]["entities"], "Unsaved triangle \u96ea")
            self.assertEqual(ready["initial"]["history"]["undo_entries"], 0)
            self.status(ready["status"], dirty=True)
            self.entities(ready["entities"], "Editor harbor", [-2, -1, 0], None)
            self.assertEqual(ready["history"]["undo_entries"], 2)
            self.assertEqual(ready["history"]["redo_entries"], 0)
            self.assertEqual(ready["metadata"], metadata)
            self.assertEqual(workflow.read_json(root / workflow.SIDECARS[0]), metadata)
            self.publication(ready["metadata_publication"])
            self.dirty_guards(ready["guards"])
            self.assertEqual((root / workflow.SCENE_PATH).read_bytes(), original_scene)
            imported = ready["import"]
            self.assertEqual(imported["previous_key"], prior_import["key"])
            self.assertNotEqual(imported["root_key"], prior_import["key"])
            self.assertEqual((imported["imported"], imported["reused"]), (1, 1))
            self.assertEqual(imported["source_digest"], prior_import["source_digest"])
            self.assertNotEqual(imported["settings_digest"], prior_import["settings_digest"])
            self.assertEqual(imported["payload_digest"], prior_import["payload_digest"])
            self.assertEqual(imported["dependencies"], prior_import["dependencies"])
            self.assertEqual(imported["nodes"], 2)
            self.publication(imported["publication"])
            # This is a real competing process, not another in-process protocol object.
            with pipes.PipeClient(scratch, root) as competitor:
                busy = competitor.call("project.open", {"path": workflow.PROJECT_PATH})
                self.assertEqual(busy["error"]["data"]["code"], "conflict", busy)
                self.assertFalse(competitor.result("project.status")["open"])
                competitor.finish()
            self.assertEqual((root / workflow.SCENE_PATH).read_bytes(), original_scene)
            closed = editor.save_close()
            self.assertEqual(closed["event"], "closed")
            self.publication(closed["save"])
            self.assertTrue(closed["save"]["current_scene_clean"])
            self.assertEqual(closed["history_before"], ready["history"])
            self.assertEqual(closed["history_after"], ready["history"])
            self.assertFalse(closed["status"]["open"])
        self.saved_entities(root, "Editor harbor", [-2, -1, 0], None)

        with pipes.PipeClient(scratch, root, workflow.PROJECT_PATH) as terminal:
            initial = terminal.result("project.status")
            self.status(initial, dirty=False)
            self.assertEqual(terminal.result("scene.history")["undo_entries"], 0)
            # Scene instances are process-local. The mutation scope, not a raw
            # scene counter, distinguishes owners across process boundaries.
            self.assertNotEqual(terminal.result("scene.mutation_status")["scope"], ready["mutation_status"]["scope"])
            page = terminal.result("scene.query", {"fields": FIELDS, "page_size": 16})
            self.assertTrue(page["complete"])
            self.entities(page["entities"], "Editor harbor", [-2, -1, 0], None)
            catalog = terminal.result("asset.catalog.status")["revision"]
            catalog = terminal.result("asset.catalog.refresh", {
                "sidecars": workflow.SIDECARS, "expected_revision": catalog})["revision"]
            cache_revision = terminal.result("asset.cache.status")["revision"]
            cache = terminal.result("asset.cache.query", {
                "asset_id": IDS["mesh"], "target": workflow.TARGET,
                "expected_catalog": catalog, "expected_cache": cache_revision})
            self.assertEqual(cache["state"], "fresh", cache)
            self.assertEqual(cache["last_valid_key"], imported["root_key"])
            descriptor = terminal.result("asset.cache.inspect", {
                "key": cache["last_valid_key"], "expected_revision": cache_revision})
            self.assertEqual(descriptor["asset_id"], IDS["mesh"])
            self.assertEqual(descriptor["target"], workflow.TARGET)
            for field in ("source_digest", "settings_digest", "payload_digest", "dependencies"):
                self.assertEqual(descriptor[field], imported[field])

            before_file = (root / workflow.SCENE_PATH).read_bytes()
            mutation = terminal.result("scene.mutation_status")
            changed = terminal.result("scene.mutate", {
                "mutation_id": mutation["next"], "expected_revision": mutation["revision"],
                "operation": {"type": "batch", "batch": {
                    "id": "16e00000-0000-4000-8000-000000000001", "commands": [
                        {"type": "set_name", "entity_id": IDS["entity"], "name": "Terminal harbor continuation"},
                        {"type": "set_transform", "entity_id": IDS["entity"], "transform": {
                            "translation": [-3, -1, 0], "rotation": [0, 0, 0, 1], "scale": [2, 2, 2]}},
                        {"type": "reparent_entity", "entity_id": IDS["entity"],
                         "parent_id": IDS["camera"], "mode": "keep_local"}]}}})
            self.assertTrue(changed["outcome"]["ok"], changed)
            self.assertTrue(changed["outcome"]["value"]["result"]["ok"], changed)
            dirty = terminal.result("project.status")
            self.status(dirty, dirty=True)
            history = terminal.result("scene.history")
            self.assertEqual(history["undo_entries"], 1)
            self.assertEqual(terminal.call("project.close")["error"]["data"]["code"], "conflict")
            busy, = self.probe(root, "busy", transcript)
            self.assertEqual((busy["event"], busy["code"]), ("busy", "conflict"))
            self.assertEqual(terminal.result("project.status"), dirty)
            self.assertEqual(terminal.result("scene.history"), history)
            self.assertEqual((root / workflow.SCENE_PATH).read_bytes(), before_file)
            saved = terminal.result("scene.save", {"expected_revision": dirty["revision"]})
            self.assertTrue(saved["success"] and saved["current_scene_clean"], saved)
            self.publication(saved["publication"])
            self.assertEqual(terminal.result("scene.history"), history)
            terminal_scope = terminal.result("scene.mutation_status")["scope"]
            self.assertFalse(terminal.result("project.close")["open"])
            terminal.finish()

        self.saved_entities(root, "Terminal harbor continuation", [-3, -1, 0], IDS["camera"])
        terminal_scene = (root / workflow.SCENE_PATH).read_bytes()
        reopened, discarded = self.probe(root, "continue", transcript)
        self.assertEqual((reopened["event"], reopened["seed"]), ("reopened", 1337))
        self.status(reopened["status"], dirty=False)
        self.assertNotEqual(reopened["mutation_status"]["scope"], terminal_scope)
        self.assertEqual(reopened["history"]["undo_entries"], 0)
        self.assertEqual(reopened["history"]["redo_entries"], 0)
        self.entities(reopened["entities"], "Terminal harbor continuation", [-3, -1, 0], IDS["camera"])
        self.assertEqual(reopened["metadata"], metadata)
        self.assertEqual(reopened["cache"]["state"], "fresh")
        self.assertEqual(reopened["cache"]["key"], imported["root_key"])
        self.assertEqual(discarded["event"], "discarded")
        self.entities(discarded["before_undo"]["entities"], "Discarded editor scratch", [-3, -1, 0], IDS["camera"])
        self.entities(discarded["after_undo"]["entities"], "Terminal harbor continuation", [-3, -1, 0], IDS["camera"])
        self.entities(discarded["after_redo"]["entities"], "Discarded editor scratch", [-3, -1, 0], IDS["camera"])
        self.assertEqual(discarded["before_undo"]["history"]["undo_entries"], 1)
        self.assertEqual(discarded["after_undo"]["history"]["redo_entries"], 1)
        self.assertEqual(discarded["after_redo"]["history"]["undo_entries"], 1)
        self.dirty_guards(discarded["guards"])
        self.assertFalse(discarded["status"]["open"])
        self.assertEqual((root / workflow.SCENE_PATH).read_bytes(), terminal_scene)
        self.assertEqual((root / workflow.PROJECT_PATH).read_bytes(), original_project)
        self.assertEqual(workflow.read_json(root / workflow.SIDECARS[0]), metadata)
        for path, contents in preserved.items():
            self.assertEqual((root / path).read_bytes(), contents, path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", type=Path, required=True)
    parser.add_argument("--editor-handoff", type=Path, required=True)
    parser.add_argument("--scratch-parent", type=Path, required=True)
    ARGS, remaining = parser.parse_known_args()
    ARGS.author = ARGS.author.resolve()
    ARGS.editor_handoff = ARGS.editor_handoff.resolve()
    ARGS.scratch_parent = ARGS.scratch_parent.resolve()
    pipes.ARGS = ARGS
    unittest.main(argv=[sys.argv[0], *remaining])
