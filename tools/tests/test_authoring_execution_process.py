"""Current-owner terminal runs and truthful CPU execution capabilities."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

import test_authoring_protocol_process as pipes


class AuthoringExecutionProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shark-terminal-execution-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "caf\u00e9 \u4e16\u754c"
        (self.root / "scenes").mkdir(parents=True)
        self.original = {}
        for relative in ("FeatureWorld.sharkproject", "scenes/FeatureWorld.sharkscene"):
            self.original[relative] = (pipes.ARGS.project.parent / relative).read_bytes()
            (self.root / relative).write_bytes(self.original[relative])
        self.scene_path = self.root / "scenes/FeatureWorld.sharkscene"

    def client(self, opened=True):
        return pipes.PipeClient(self.root, self.root,
                                "FeatureWorld.sharkproject" if opened else None)

    def assert_sources_unchanged(self):
        for relative, expected in self.original.items():
            self.assertEqual((self.root / relative).read_bytes(), expected, relative)

    def rpc_error(self, row, code=None):
        self.assertIn("error", row, row)
        self.assertTrue(row["error"]["message"])
        self.assertIsInstance(row["error"]["data"]["context"], dict)
        if code is not None:
            self.assertEqual(row["error"]["data"]["code"], code, row)

    @staticmethod
    def semantic_report(report):
        result = copy.deepcopy(report)
        for case in result["cases"]:
            for key in ("initial_revision", "final_revision"):
                if case[key] is not None:
                    del case[key]["instance"]
        return result

    def test_dirty_snapshot_runs_preserve_owner_and_match_saved_digest(self):
        with self.client() as client:
            initial = client.result("project.status")
            baseline = client.result("scene.run", {
                "expected_revision": initial["revision"], "ticks": 2})
            self.assertFalse(baseline["dirty"])
            state = client.result("scene.mutation_status")
            intent = {"mutation_id": state["next"], "expected_revision": state["revision"],
                      "operation": {"type": "command", "command": {
                          "type": "set_name", "entity_id": pipes.ENTITIES[0],
                          "name": "Unsaved run \u96ea"}}}
            edited = client.result("scene.mutate", intent)
            self.assertTrue(edited["outcome"]["ok"], edited)
            self.assertTrue(edited["outcome"]["value"]["result"]["ok"], edited)
            before = client.result("project.status")
            history = client.result("scene.history")
            mutations = client.result("scene.mutation_status")
            retained = client.result("scene.mutation_outcome", {"mutation_id": intent["mutation_id"]})
            self.assertTrue(before["dirty"])
            self.assertEqual(history["undo_entries"], 1)
            parameters = {"expected_revision": before["revision"], "seed": "18446744073709551615",
                          "ticks": 7, "hz": 59, "run_id": "dirty-snapshot",
                          "revision": "fixture-source", "expected_entities": 4}
            first = client.result("scene.run", parameters)
            second = client.result("scene.run", parameters)
            self.assertEqual(first["observed_revision"], before["revision"])
            self.assertTrue(first["dirty"])
            self.assertEqual(first["entity_count"], 4)
            self.assertRegex(first["source_digest"], r"^[0-9a-f]{64}$")
            self.assertNotEqual(first["source_digest"], baseline["source_digest"])
            for key in ("observed_revision", "dirty", "entity_count", "source_digest"):
                self.assertEqual(first[key], second[key], key)
            self.assertEqual(self.semantic_report(first["report"]),
                             self.semantic_report(second["report"]))
            report = first["report"]
            self.assertEqual((report["format"], report["schema_version"], report["status"]),
                             ("shark.scene-run", 1, "PASS"))
            self.assertEqual((report["mode"], report["renderer_initialized"]), ("cpu", False))
            self.assertEqual((report["project_id"], report["scene_id"]), (pipes.PROJECT, pipes.SCENE))
            self.assertEqual((report["seed"], report["ticks"], report["tick_rate_hz"]),
                             (18446744073709551615, 7, 59))
            self.assertEqual((report["run_id"], report["revision"]),
                             ("dirty-snapshot", "fixture-source"))
            self.assertEqual(report["inputs"], [])
            self.assertEqual(report["artifacts"], [])
            self.assertEqual(len(report["cases"]), 1)
            case = report["cases"][0]
            self.assertEqual((case["id"], case["status"], case["ticks_completed"]),
                             ("scene-integrity", "PASS", 7))
            self.assertGreater(case["assertions"], 7)
            self.assertEqual(case["random_draws"], 0)
            self.assertIsNone(case["failure"])
            self.assertEqual(case["initial_revision"], case["final_revision"])
            self.assertEqual(client.result("project.status"), before)
            self.assertEqual(client.result("scene.history"), history)
            self.assertEqual(client.result("scene.mutation_status"), mutations)
            self.assertEqual(client.result("scene.mutation_outcome", {
                "mutation_id": intent["mutation_id"]}), retained)
            self.assert_sources_unchanged()

            # Only this explicit save writes source. Its complete bytes independently
            # bind the run's unsaved snapshot digest to the canonical public format.
            saved = client.result("scene.save", {"expected_revision": before["revision"]})
            self.assertTrue(saved["success"] and saved["current_scene_clean"], saved)
            saved_bytes = self.scene_path.read_bytes()
            self.assertEqual(first["source_digest"], hashlib.sha256(saved_bytes).hexdigest())
            document = json.loads(saved_bytes)
            root = next(row for row in document["entities"] if row["entity_id"] == pipes.ENTITIES[0])
            self.assertEqual(root["components"]["shark.name"]["value"], "Unsaved run \u96ea")
            self.assertEqual(client.result("scene.history"), history)
            client.result("project.close")
            client.finish()

    def test_failed_assertion_and_invalid_runs_preserve_owner(self):
        with self.client() as client:
            before = client.result("project.status")
            history = client.result("scene.history")
            mutations = client.result("scene.mutation_status")
            failed = client.result("scene.run", {
                "expected_revision": before["revision"], "ticks": 3, "expected_entities": 5})
            report = failed["report"]
            self.assertEqual(report["status"], "FAIL")
            case = report["cases"][0]
            self.assertEqual((case["status"], case["ticks_completed"]), ("FAIL", 0))
            self.assertEqual((case["failure"]["kind"], case["failure"]["phase"]),
                             ("assertion", "setup"))
            self.assertIsNone(case["failure"]["tick"])
            self.assertIn("expected_entities", case["failure"]["error"]["message"])
            self.assertEqual(failed["entity_count"], 4)
            self.rpc_error(client.call("scene.run", {}))
            stale = {**before["revision"], "revision": str(int(before["revision"]["revision"]) + 1)}
            self.rpc_error(client.call("scene.run", {"expected_revision": stale}), "conflict")
            invalid = [("ticks", 0), ("ticks", 10001), ("ticks", True),
                       ("hz", 0), ("hz", 1001), ("seed", 1337), ("seed", "01"),
                       ("seed", "18446744073709551616"), ("expected_entities", -1),
                       ("expected_entities", 4097), ("run_id", "has space"),
                       ("revision", ""), ("inputs", []), ("unknown", True)]
            for field, value in invalid:
                with self.subTest(field=field, value=value):
                    self.rpc_error(client.call("scene.run", {
                        "expected_revision": before["revision"], field: value}))
            self.assertEqual(client.result("project.status"), before)
            self.assertEqual(client.result("scene.history"), history)
            self.assertEqual(client.result("scene.mutation_status"), mutations)
            self.assert_sources_unchanged()
            client.result("project.close")
            self.rpc_error(client.call("scene.run", {"expected_revision": before["revision"]}), "conflict")
            client.finish()

    def test_cpu_host_advertises_only_available_execution(self):
        with self.client() as client:
            before = client.result("project.status")
            discovery = client.result("protocol.discover")
            methods = {row["name"] for row in discovery["methods"]}
            self.assertIn("scene.run", methods)
            self.assertNotIn("scene.export", methods)
            self.assertNotIn("scene.capture", methods)
            run_schema = next(row for row in discovery["methods"] if row["name"] == "scene.run")
            for field in ("run_id", "revision"):
                self.assertEqual(run_schema["params"]["properties"][field]["pattern"],
                                 "^[A-Za-z0-9._-]{1,64}$")
            capabilities = discovery["capabilities"]
            self.assertTrue(capabilities["scene_runs"])
            self.assertFalse(capabilities["standalone_export"])
            self.assertFalse(capabilities["device_capture"])
            for method in ("scene.export", "scene.capture"):
                self.rpc_error(client.call(method, {"expected_revision": before["revision"]}),
                               "capability_unavailable")
            self.assertEqual(client.result("project.status"), before)
            self.assert_sources_unchanged()
            client.finish()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", type=Path, required=True)
    parser.add_argument("--project", type=Path,
                        default=pipes.ROOT / "assets/FeatureWorld/FeatureWorld.sharkproject")
    args, remaining = parser.parse_known_args()
    args.author, args.project = args.author.resolve(), args.project.resolve()
    pipes.ARGS = args
    unittest.main(argv=[sys.argv[0], *remaining])
