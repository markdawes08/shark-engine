"""Terminal edits, retained outcomes, persistence and scoped text through real pipes."""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

import test_authoring_protocol_process as pipes

ENTITY = "30000000-0000-4000-8000-000000000001"
CHILD = "30000000-0000-4000-8000-000000000002"
BATCH = "30000000-0000-4000-8000-000000000003"
ASSET = "30000000-0000-4000-8000-000000000004"
TRANSFORM = {"translation": [3, 4, 5], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}
CAMERA = {"projection": "perspective", "vertical_fov_radians": 1.0,
          "orthographic_height": 10.0, "near_clip": 0.1, "far_clip": 100.0}
MESH = {"asset_id": ASSET, "mesh_index": 0, "visible": True}


class AuthoringEditingProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shark-terminal-edits-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "scenes").mkdir()
        (self.root / "scripts").mkdir()
        self.project_path = self.root / "FeatureWorld.sharkproject"
        self.scene_path = self.root / "scenes/FeatureWorld.sharkscene"
        for path in (self.project_path, self.scene_path):
            path.write_bytes((pipes.ARGS.project.parent / path.relative_to(self.root)).read_bytes())

    def client(self, opened=True):
        return pipes.PipeClient(self.root, self.root, "FeatureWorld.sharkproject" if opened else None)

    def mutation(self, client, operation, expected=None):
        state = client.result("scene.mutation_status")
        return {"mutation_id": state["next"], "expected_revision": expected or state["revision"],
                "operation": operation}

    def edit(self, client, command):
        intent = self.mutation(client, {"type": "command", "command": command})
        result = client.result("scene.mutate", intent)
        self.effect(result)
        return result

    def effect(self, result, *, ok=True):
        self.assertTrue(result["outcome"]["ok"], result)
        original = result["outcome"]["value"]
        self.assertIs(original["result"]["ok"], ok, result)
        return original["result"].get("value")

    def rpc_error(self, result, code=None):
        self.assertIn("error", result, result)
        if code is not None:
            self.assertEqual(result["error"]["data"]["code"], code, result)

    def inspect(self, client, entity=ENTITY):
        return client.result("scene.inspect", {"entity_id": entity,
                             "fields": ["name", "transform", "hierarchy", "camera", "mesh_renderer"]})["entities"][0]

    def save(self, client):
        revision = client.result("project.status")["revision"]
        saved = client.result("scene.save", {"expected_revision": revision})
        self.assertTrue(saved["success"] and saved["current_scene_clean"], saved)
        self.assertEqual(saved["saved_revision"], revision)
        self.assertTrue(saved["publication"]["published"])
        self.assertIn(saved["publication"]["durability"],
                      {"file_flushed", "file_and_directory_flushed", "directory_flush_failed"})
        return saved

    def test_preview_edit_history_save_and_reopen(self):
        with self.client() as client:
            base = client.result("project.status")
            original_bytes = self.scene_path.read_bytes()
            batch = {"id": BATCH, "commands": [
                {"type": "create_entity", "entity_id": ENTITY, "name": "Terminal camera",
                 "transform": TRANSFORM, "camera": CAMERA, "mesh_renderer": MESH},
                {"type": "create_entity", "entity_id": CHILD, "name": "Child", "parent_id": ENTITY}]}
            preview = client.result("scene.preview", {"batch": batch, "expected_revision": base["revision"]})
            self.assertTrue(preview["outcome"]["ok"])
            self.assertTrue(preview["outcome"]["value"]["changed"])
            self.assertEqual(preview["observed_revision"], base["revision"])
            self.assertEqual(client.result("project.status"), base)
            self.assertEqual(self.scene_path.read_bytes(), original_bytes)
            intent = self.mutation(client, {"type": "batch", "batch": batch})
            committed = client.result("scene.mutate", intent)
            self.assertEqual(set(self.effect(committed)["affected_entities"]), {ENTITY, CHILD})
            self.assertEqual(int(committed["observed_revision"]["revision"]),
                             int(base["revision"]["revision"]) + 1)
            row = self.inspect(client)
            self.assertEqual(row["name"], "Terminal camera")
            self.assertEqual(row["transform"]["local"], TRANSFORM)
            self.assertEqual(row["camera"], CAMERA)
            self.assertEqual(row["mesh_renderer"], MESH)
            self.assertEqual(self.inspect(client, CHILD)["hierarchy"]["parent_id"], ENTITY)
            self.rpc_error(client.call("project.close"), "conflict")
            self.edit(client, {"type": "set_name", "entity_id": ENTITY, "name": "Harbor \u96ea"})
            self.edit(client, {"type": "set_transform", "entity_id": ENTITY,
                               "transform": {**TRANSFORM, "translation": [7, 8, 9]}})
            self.edit(client, {"type": "reparent_entity", "entity_id": CHILD,
                               "parent_id": None, "mode": "keep_world"})
            self.assertEqual(self.inspect(client, CHILD)["transform"]["local"]["translation"], [7, 8, 9])
            self.edit(client, {"type": "set_camera", "entity_id": ENTITY, "camera": None})
            self.edit(client, {"type": "set_mesh_renderer", "entity_id": ENTITY, "mesh_renderer": None})
            self.assertIsNone(self.inspect(client)["camera"])
            self.assertIsNone(self.inspect(client)["mesh_renderer"])
            history = client.result("scene.history")
            self.save(client)
            self.assertEqual(client.result("scene.history"), history)
            undo = self.mutation(client, {"type": "undo", "ticket": history["next_undo"]["ticket"]})
            self.effect(client.result("scene.mutate", undo))
            self.assertEqual(self.inspect(client)["mesh_renderer"], MESH)
            history = client.result("scene.history")
            redo = self.mutation(client, {"type": "redo", "ticket": history["next_redo"]["ticket"]})
            self.effect(client.result("scene.mutate", redo))
            self.assertIsNone(self.inspect(client)["mesh_renderer"])
            self.edit(client, {"type": "delete_entity", "entity_id": CHILD})
            clear = self.mutation(client, {"type": "clear_history"})
            cleared = client.result("scene.mutate", clear)
            self.effect(cleared)
            self.assertEqual(cleared["outcome"]["value"]["history_after"]["undo_entries"], 0)
            self.save(client)
            old_revision = client.result("project.status")["revision"]
            client.result("project.close")
            opened = client.result("project.open", {"path": "FeatureWorld.sharkproject"})
            self.assertNotEqual(opened["revision"]["scene_instance"], old_revision["scene_instance"])
            self.assertEqual(self.inspect(client)["name"], "Harbor \u96ea")
            self.rpc_error(client.call("scene.inspect", {"entity_id": CHILD}))
            client.finish()
        saved = json.loads(self.scene_path.read_text(encoding="utf-8"))
        self.assertIn("Harbor \u96ea", json.dumps(saved, ensure_ascii=False))

    def test_retained_replay_mismatch_rejection_expiry_and_restart(self):
        original_bytes = self.scene_path.read_bytes()
        with self.client() as client:
            intent = self.mutation(client, {"type": "command", "command": {
                "type": "set_name", "entity_id": pipes.ENTITIES[0], "name": "Lost reply"}})
            client.send(pipes.encoded(pipes.request("scene.mutate", intent, "discard-me")) + b"\n")
            client.row()  # Delivery was intentionally ignored by this caller.
            replay = client.result("scene.mutate", intent)
            self.assertEqual(replay["state"], "replayed")
            self.assertTrue(self.effect(replay)["changed"])
            self.assertEqual(client.result("scene.history")["undo_entries"], 1)
            lookup = client.result("scene.mutation_outcome", {"mutation_id": intent["mutation_id"]})
            self.assertEqual(lookup["state"], "retained")
            self.assertEqual(lookup["outcome"], replay["outcome"])
            different = copy.deepcopy(intent)
            different["operation"]["command"]["name"] = "Wrong replacement"
            mismatch = client.result("scene.mutate", different)
            self.assertEqual(mismatch["state"], "payload_mismatch")
            self.assertFalse(mismatch["outcome"]["ok"])
            rejected = self.mutation(client, intent["operation"], intent["expected_revision"])
            failure = client.result("scene.mutate", rejected)
            self.assertEqual(failure["state"], "recorded")
            self.effect(failure, ok=False)
            self.assertEqual(client.result("scene.mutate", rejected)["state"], "replayed")
            state = client.result("scene.mutation_status")
            future = self.mutation(client, intent["operation"])
            future["mutation_id"]["sequence"] = str(int(state["next"]["sequence"]) + 1)
            self.assertEqual(client.result("scene.mutate", future)["state"], "out_of_order")
            # Native retention defaults to 32 entries; evicted outcomes stay unknown.
            for _ in range(33):
                self.edit(client, intent["operation"]["command"])
            expired = client.result("scene.mutation_outcome", {"mutation_id": intent["mutation_id"]})
            self.assertEqual(expired["state"], "unknown_outcome")
            self.assertLessEqual(client.result("scene.mutation_status")["retained_entries"], 32)
            client.finish()  # Dirty EOF releases the lock; it never saves automatically.
        self.assertEqual(self.scene_path.read_bytes(), original_bytes)
        with self.client() as client:
            foreign = client.result("scene.mutate", intent)
            self.assertEqual(foreign["state"], "unknown_outcome")
            self.assertFalse(client.result("project.status")["dirty"])
            client.finish()

    def test_atomic_validation_and_revision_bound_cursors(self):
        with self.client() as client:
            base = client.result("project.status")
            page = client.result("scene.query", {"page_size": 1})
            batch = {"id": BATCH, "commands": [
                {"type": "set_name", "entity_id": pipes.ENTITIES[0], "name": "Must roll back"},
                {"type": "reparent_entity", "entity_id": pipes.ENTITIES[0],
                 "parent_id": pipes.ENTITIES[0], "mode": "keep_local"}]}
            intent = self.mutation(client, {"type": "batch", "batch": batch})
            self.effect(client.result("scene.mutate", intent), ok=False)
            self.assertEqual(client.result("project.status")["revision"], base["revision"])
            self.assertEqual(self.inspect(client, pipes.ENTITIES[0])["name"], "Root")
            self.assertEqual(client.result("scene.history")["undo_entries"], 0)
            # Unknown command fields reject at the wire without consuming native sequence.
            state = client.result("scene.mutation_status")
            malformed = self.mutation(client, {"type": "command", "command": {
                "type": "delete_entity", "entity_id": pipes.ENTITIES[0], "typo": True}})
            self.rpc_error(client.call("scene.mutate", malformed))
            self.assertEqual(client.result("scene.mutation_status"), state)
            continued = client.result("scene.continue", {"cursor": page["next_cursor"]})
            self.edit(client, {"type": "set_name", "entity_id": pipes.ENTITIES[0], "name": "Changed"})
            self.rpc_error(client.call("scene.continue", {"cursor": continued["next_cursor"]}), "conflict")
            self.rpc_error(client.call("scene.history", {"expected_revision": base["revision"]}), "conflict")
            client.finish()

    def test_external_edits_save_conflicts_and_explicit_reload(self):
        with self.client() as client:
            self.edit(client, {"type": "set_name", "entity_id": pipes.ENTITIES[0], "name": "Unsaved"})
            status = client.result("project.status")
            external = self.scene_path.read_bytes() + b"\n"
            self.scene_path.write_bytes(external)
            self.rpc_error(client.call("scene.save", {"expected_revision": status["revision"]}), "conflict")
            self.rpc_error(client.call("scene.reload", {"expected_revision": status["revision"]}), "conflict")
            self.assertEqual(self.scene_path.read_bytes(), external)
            self.assertEqual(self.inspect(client, pipes.ENTITIES[0])["name"], "Unsaved")
            scope = client.result("scene.mutation_status")["scope"]
            reloaded = client.result("scene.reload", {"expected_revision": status["revision"], "unsaved": "discard"})
            self.assertFalse(reloaded["dirty"])
            self.assertNotEqual(reloaded["revision"]["scene_instance"], status["revision"]["scene_instance"])
            self.assertEqual(client.result("scene.mutation_status")["scope"], scope)
            self.assertEqual(client.result("scene.history")["undo_entries"], 0)
            self.assertEqual(self.inspect(client, pipes.ENTITIES[0])["name"], "Root")
            # Equal semantic content with different bytes is still an external project edit.
            project_bytes = self.project_path.read_bytes()
            self.project_path.write_bytes(project_bytes + b"\n")
            self.rpc_error(client.call("scene.save", {"expected_revision": reloaded["revision"]}), "conflict")
            self.rpc_error(client.call("scene.reload", {"expected_revision": reloaded["revision"]}), "conflict")
            self.rpc_error(client.call("text.write", {"path": "scripts/demo.lua", "text": "return 1\n",
                                                       "expected_revision": None}), "conflict")
            self.assertFalse((self.root / "scripts/demo.lua").exists())
            self.project_path.write_bytes(project_bytes)
            self.save(client)
            self.edit(client, {"type": "set_name", "entity_id": pipes.ENTITIES[0], "name": "Discard me"})
            dirty = client.result("project.status")
            self.project_path.write_bytes(project_bytes + b"\n")
            self.rpc_error(client.call("project.close", {"unsaved": "discard"}))
            self.rpc_error(client.call("project.close", {"unsaved": "discard",
                                                          "expected_revision": reloaded["revision"]}), "conflict")
            self.assertTrue(client.result("project.status")["dirty"])
            self.assertEqual(client.result("project.close", {"unsaved": "discard",
                                                               "expected_revision": dirty["revision"]}), {"open": False})
            client.result("project.open", {"path": "FeatureWorld.sharkproject"})
            self.assertEqual(self.inspect(client, pipes.ENTITIES[0])["name"], "Root")
            client.finish()

    def test_scoped_text_revision_conflict_and_typed_path_protection(self):
        with self.client() as client:
            initial = client.result("project.status")
            created = client.result("text.write", {"path": "scripts/demo.lua", "text": "-- \u96ea\nreturn 1\n",
                                                    "expected_revision": None})
            self.assertTrue(created["publication"]["published"])
            read = client.result("text.read", {"path": "scripts/demo.lua"})
            self.assertEqual(read["text"], "-- \u96ea\nreturn 1\n")
            self.rpc_error(client.call("text.write", {"path": "scripts/other.lua", "text": "wrong",
                                                       "expected_revision": read["revision"]}), "conflict")
            self.rpc_error(client.call("text.write", {"path": "scripts/demo.lua", "text": "wrong",
                                                       "expected_revision": None}), "conflict")
            (self.root / "scripts/demo.lua").write_bytes(b"-- external\n")
            self.rpc_error(client.call("text.write", {"path": "scripts/demo.lua", "text": "wrong",
                                                       "expected_revision": read["revision"]}), "conflict")
            fresh = client.result("text.read", {"path": "scripts/demo.lua"})
            written = client.result("text.write", {"path": "scripts/demo.lua", "text": "return 2\n",
                                                    "expected_revision": fresh["revision"]})
            self.assertNotEqual(written["revision"], fresh["revision"])
            self.assertEqual((self.root / "scripts/demo.lua").read_bytes(), b"return 2\n")
            for path in ("../escape.lua", "FeatureWorld.sharkproject", "scenes/FeatureWorld.sharkscene",
                         "scripts/.shark-write.lock", "scripts/binary.bin", "Scripts/alias.lua"):
                self.rpc_error(client.call("text.write", {"path": path, "text": "bad",
                                                           "expected_revision": None}))
            self.rpc_error(client.call("text.write", {"path": "scripts/nul.lua", "text": "\0",
                                                       "expected_revision": None}))
            # A full valid script exceeds the older source-JSON 4096-byte string bound.
            large = pipes.request("text.write", {"path": "scripts/large.lua", "text": "x" * 32768,
                                                  "expected_revision": None}, "large")
            writer, failures = client.send_large(pipes.encoded(large) + b"\n")
            self.assertIn("result", client.row())
            writer.join(timeout=1)
            self.assertFalse(writer.is_alive() or failures)
            self.assertEqual((self.root / "scripts/large.lua").stat().st_size, 32768)
            self.assertEqual(client.result("project.status")["revision"], initial["revision"])
            self.assertFalse(client.result("project.status")["dirty"])
            self.assertEqual(client.result("scene.history")["undo_entries"], 0)
            # Four immutable text observations remain; pressure never silently evicts them.
            for _ in range(28):
                client.result("text.read", {"path": "scripts/demo.lua"})
            self.rpc_error(client.call("text.read", {"path": "scripts/demo.lua"}), "conflict")
            released = client.result("text.release", {"revision": created["revision"]})
            self.assertTrue(released["released"])
            self.rpc_error(client.call("text.release", {"revision": created["revision"]}), "conflict")
            self.assertEqual(client.result("text.read", {"path": "scripts/demo.lua"})["text"], "return 2\n")
            client.finish()
        # Source kinds use headers, not extensions: a declared scene may end in .lua.
        project = json.loads(self.project_path.read_text(encoding="utf-8"))
        project["scenes"][0]["path"] = "scripts/typed.lua"
        (self.root / "scripts/typed.lua").write_bytes(self.scene_path.read_bytes())
        self.project_path.write_text(json.dumps(project), encoding="utf-8")
        with self.client() as client:
            self.rpc_error(client.call("text.write", {"path": "scripts/typed.lua", "text": "return 0",
                                                       "expected_revision": None}), "conflict")
            self.rpc_error(client.call("text.read", {"path": "scripts/typed.lua"}), "conflict")
            client.finish()

    def test_project_creation_and_existing_file_preservation(self):
        params = {"path": "New.sharkproject", "scene_path": "scenes/New.sharkscene", "name": "New world",
                  "project_id": "40000000-0000-4000-8000-000000000001",
                  "world_id": "40000000-0000-4000-8000-000000000002",
                  "scene_id": "40000000-0000-4000-8000-000000000003"}
        with self.client(opened=False) as client:
            created = client.result("project.create", params)
            self.assertTrue(created["success"], created)
            self.assertTrue(created["published"]["scene"]["published"])
            self.assertTrue(created["published"]["project"]["published"])
            status = client.result("project.status")
            self.assertEqual(status["project_id"], params["project_id"])
            self.assertFalse(status["dirty"])
            self.assertEqual(client.result("scene.query")["entities"], [])
            self.edit(client, {"type": "create_entity", "entity_id": ENTITY, "name": "Start"})
            self.save(client)
            client.result("project.close")
            before = (self.root / "New.sharkproject").read_bytes()
            failed = client.call("project.create", params)
            self.assertTrue("error" in failed or not failed["result"]["success"], failed)
            self.assertEqual((self.root / "New.sharkproject").read_bytes(), before)
            self.assertEqual(client.result("project.status"), {"open": False})
            client.result("project.open", {"path": params["path"]})
            self.assertEqual(self.inspect(client)["name"], "Start")
            client.finish()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", type=Path, required=True)
    parser.add_argument("--project", type=Path, default=pipes.ROOT / "assets/FeatureWorld/FeatureWorld.sharkproject")
    args, remaining = parser.parse_known_args()
    args.author, args.project = args.author.resolve(), args.project.resolve()
    pipes.ARGS = args
    unittest.main(argv=[sys.argv[0], *remaining])
