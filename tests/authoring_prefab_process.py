"""Create prefab instances through real public pipes and continue a saved project."""

import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/tests"))
import test_authoring_protocol_process as pipes


PROJECT = "21d00000-0000-4000-8000-000000000001"
WORLD = "21d00000-0000-4000-8000-000000000002"
SCENE = "21d00000-0000-4000-8000-000000000003"
ROOT = "21d00000-0000-4000-8000-000000000010"
CHILD = "21d00000-0000-4000-8000-000000000011"
ASSET = "21d00000-0000-4000-8000-000000000020"
BATCH = "21d00000-0000-4000-8000-000000000030"
PREFAB = "assets/Tree.sharkprefab"


class Client(pipes.PipeClient):
    def call(self, method, params=None):
        identity = self.next_id
        self.next_id += 1
        body = pipes.encoded(pipes.request(method, params, identity)) + b"\n"
        assert len(body) <= pipes.REQUEST_BYTES + 1
        writer = None
        failures = []
        if len(body) > 2048:
            writer, failures = self.send_large(body)
        else:
            self.send(body)
        row = self.row()
        if writer is not None:
            writer.join(timeout=self.timeout)
            assert not writer.is_alive() and not failures, failures
        assert row["id"] == identity, row
        return row


class AuthoringPrefabProcessTests(unittest.TestCase):
    def effect(self, reply):
        self.assertTrue(reply["outcome"]["ok"], reply)
        original = reply["outcome"]["value"]["result"]
        self.assertTrue(original["ok"], reply)
        self.assertTrue(original["value"]["changed"], reply)

    def mutate(self, client, operation):
        status = client.result("scene.mutation_status")
        return client.result("scene.mutate", {"mutation_id": status["next"],
                             "expected_revision": status["revision"], "operation": operation})

    def save(self, client):
        revision = client.result("project.status")["revision"]
        reply = client.result("scene.save", {"expected_revision": revision})
        self.assertTrue(reply["success"] and reply["current_scene_clean"], reply)
        self.assertTrue(reply["publication"]["published"], reply)

    def test_saved_instances_continue_in_a_fresh_terminal_owner(self):
        with tempfile.TemporaryDirectory(prefix="shark-prefab-process-") as temporary:
            root = Path(temporary) / "harbor \u96ea"
            (root / "assets").mkdir(parents=True)
            with Client(root, root) as client:
                created = client.result("project.create", {
                    "path": "Project.sharkproject", "scene_path": "Scene.sharkscene",
                    "name": "Prefab handoff", "project_id": PROJECT,
                    "world_id": WORLD, "scene_id": SCENE})
                self.assertTrue(created["success"], created)
                self.effect(self.mutate(client, {"type": "batch", "batch": {
                    "id": BATCH, "commands": [
                        {"type": "create_entity", "entity_id": ROOT, "name": "Tree"},
                        {"type": "create_entity", "entity_id": CHILD,
                         "name": "Branch", "parent_id": ROOT}]}}))
                revision = client.result("project.status")["revision"]
                captured = client.result("prefab.capture", {
                    "entity_id": ROOT, "asset_id": ASSET, "expected_revision": revision})
                self.assertEqual(captured["entity_count"], 2)
                self.assertEqual(client.result("project.status")["revision"], revision)
                metadata = {"format": "shark.asset", "schema_version": 1,
                            "project_id": PROJECT, "asset_id": ASSET, "kind": "prefab",
                            "origin": "Shark authored offline fixture", "license": "MIT",
                            "importer": "shark.prefab", "audience": "shared",
                            "settings": {}, "dependencies": [], "extras": {}}
                sidecar = client.result("asset.metadata.write", {
                    "path": PREFAB + ".sharkasset", "metadata": metadata,
                    "expected_revision": None})
                self.assertTrue(sidecar["publication"]["published"])
                source = client.result("prefab.write", {"path": PREFAB,
                    "document": captured["document"], "expected_revision": None})
                self.assertTrue(source["publication"]["published"])
                source_bytes = (root / PREFAB).read_bytes()
                prepared = client.result("prefab.prepare", {
                    "revision": source["revision"], "expected_revision": revision,
                    "parent_id": None, "overrides": [{"entity_id": ROOT, "name": "Copy \u6865"}]})
                mapping = {entry["source"]: entry["instance"] for entry in prepared["remap"]}
                self.assertEqual(set(mapping), {ROOT, CHILD})
                self.assertTrue(set(mapping.values()).isdisjoint({ROOT, CHILD}))
                mutation = client.result("scene.mutation_status")["next"]
                request = {"preparation": prepared["preparation"], "mutation_id": mutation}
                committed = client.result("prefab.instantiate", request)
                self.effect(committed)
                replay = client.result("prefab.instantiate", request)
                self.assertEqual(replay["state"], "replayed")
                self.assertEqual(replay["outcome"], committed["outcome"])
                history = client.result("scene.history")
                self.assertEqual(history["undo_entries"], 2)
                self.assertEqual(history["next_undo"]["command_count"], 1)
                self.save(client)
                client.result("project.close")
                client.finish()

            saved = json.loads((root / "Scene.sharkscene").read_text(encoding="utf-8"))
            entities = {entry["entity_id"]: entry for entry in saved["entities"]}
            self.assertEqual(set(entities), {ROOT, CHILD, *mapping.values()})
            self.assertEqual(entities[mapping[CHILD]]["parent"], mapping[ROOT])
            self.assertEqual(entities[mapping[ROOT]]["components"]["shark.name"]["value"], "Copy \u6865")
            with Client(root, root, "Project.sharkproject") as client:
                self.assertEqual(client.result("scene.history")["undo_entries"], 0)
                inspected = client.result("scene.inspect", {"entity_id": mapping[CHILD],
                    "fields": ["name", "hierarchy"]})["entities"][0]
                self.assertEqual(inspected["name"], "Branch")
                self.assertEqual(inspected["hierarchy"]["parent_id"], mapping[ROOT])
                expired = client.call("prefab.instantiate", request)
                self.assertIn("error", expired)
                self.effect(self.mutate(client, {"type": "command", "command": {
                    "type": "set_name", "entity_id": mapping[ROOT], "name": "Continued"}}))
                self.save(client)
                client.result("project.close")
                client.finish()
            self.assertEqual((root / PREFAB).read_bytes(), source_bytes)
            continued = json.loads((root / "Scene.sharkscene").read_text(encoding="utf-8"))
            names = {entry["entity_id"]: entry["components"]["shark.name"]["value"]
                     for entry in continued["entities"]}
            self.assertEqual(names[mapping[ROOT]], "Continued")
            self.assertEqual(names[ROOT], "Tree")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", type=Path, required=True)
    parser.add_argument("--project", type=Path,
                        default=pipes.ROOT / "assets/FeatureWorld/FeatureWorld.sharkproject")
    args, remaining = parser.parse_known_args()
    args.author, args.project = args.author.resolve(), args.project.resolve()
    pipes.ARGS = args
    unittest.main(argv=[sys.argv[0], *remaining])
