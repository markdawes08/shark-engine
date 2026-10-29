"""Retained terminal imports, native cache publication and process ownership."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
import uuid

import test_authoring_protocol_process as pipes

TARGET = {"id": "terminal-fixture", "side": "client"}
TERMINAL = {"succeeded", "failed", "cancelled"}
FIXTURES = pipes.ROOT / "tests/fixtures/gltf_source"


def asset_id(number):
    return "60000000-0000-4000-8000-" + format(number, "012x")


class AuthoringImportProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shark-terminal-imports-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "scenes").mkdir()
        (self.root / "assets").mkdir()
        self.original = {}
        for relative in ("FeatureWorld.sharkproject", "scenes/FeatureWorld.sharkscene"):
            self.original[relative] = (pipes.ARGS.project.parent / relative).read_bytes()
            (self.root / relative).write_bytes(self.original[relative])
        self.sidecars = []
        self.cache_path = self.root / ".shark-development-cache"

    def tearDown(self):
        for relative, expected in self.original.items():
            self.assertEqual((self.root / relative).read_bytes(), expected, relative)

    def client(self, opened=True):
        return pipes.PipeClient(self.root, self.root, "FeatureWorld.sharkproject" if opened else None)

    def rpc_error(self, row, code=None):
        self.assertIn("error", row, row)
        detail = row["error"]["data"]
        self.assertTrue(row["error"]["message"], row)
        self.assertIsInstance(detail["context"], dict)
        if code is not None:
            self.assertEqual(detail["code"], code, row)

    def publication(self, receipt):
        self.assertTrue(receipt["published"], receipt)
        self.assertGreater(receipt["bytes"], 0)
        self.assertIn(receipt["durability"],
                      {"file_flushed", "file_and_directory_flushed", "directory_flush_failed"})

    def add_asset(self, client, number, name, source, *, kind="bytes", audience="shared",
                  settings=None, dependencies=()):
        path = "assets/" + name
        if source is not None:
            (self.root / path).write_bytes(source)
        metadata = {"format": "shark.asset", "schema_version": 1, "project_id": pipes.PROJECT,
                    "asset_id": asset_id(number), "kind": kind,
                    "origin": "Shark authored offline fixture", "license": "MIT",
                    "importer": "shark." + kind, "audience": audience,
                    "settings": settings or {}, "dependencies": list(dependencies), "extras": {}}
        sidecar = path + ".sharkasset"
        result = client.result("asset.metadata.write", {
            "path": sidecar, "metadata": metadata, "expected_revision": None})
        self.publication(result["publication"])
        self.assertEqual(result["path"], sidecar)
        self.sidecars.append(sidecar)
        return metadata

    def refresh(self, client, sidecars=None):
        revision = client.result("asset.catalog.status")["revision"]
        return client.result("asset.catalog.refresh", {
            "sidecars": self.sidecars if sidecars is None else sidecars,
            "expected_revision": revision})["revision"]

    def inputs(self, client, number=1):
        return {"asset_id": asset_id(number), "target": TARGET,
                "expected_catalog": client.result("asset.catalog.status")["revision"],
                "expected_cache": client.result("asset.cache.status")["revision"]}

    def submit(self, client, number=1):
        result = client.result("asset.jobs.submit", self.inputs(client, number))
        self.assertFalse(result["deduplicated"])
        return result["job_id"]

    def ready(self, client, job_id):
        # Listing observes computation without publishing. The ready state, not
        # elapsed time, establishes every external-edit/cancellation ordering.
        deadline = time.monotonic() + 15
        completed = 0
        while time.monotonic() < deadline:
            jobs = client.result("asset.jobs.list")["jobs"]
            rows = [row for row in jobs if row["job_id"] == job_id]
            self.assertEqual(len(rows), 1, jobs)
            row = rows[0]
            self.assertLessEqual(completed, row["completed_nodes"])
            completed = row["completed_nodes"]
            self.assertLessEqual(completed, row["total_nodes"])
            self.assertLessEqual(row["total_nodes"], 128)
            if row["state"] == "ready":
                self.assertIsNone(row["commit"])
                return row
            self.assertNotIn(row["state"], TERMINAL, row)
        self.fail("Import did not reach ready before the bounded observation deadline")

    def drain(self, client, job_id, state="succeeded"):
        result = client.result("asset.jobs.drain")
        self.assertEqual(result["drained"], 1, result)
        row = next(row for row in result["jobs"] if row["job_id"] == job_id)
        self.assertEqual(row["state"], state, row)
        if state == "succeeded":
            self.assertIsNone(row["error"])
            self.assertIsNotNone(row["summary"])
            self.assertIsNotNone(row["commit"])
            if row["commit"]["changed"]:
                self.publication(row["commit"]["publication"])
        else:
            self.assertIsNone(row["commit"])
        return row

    def import_asset(self, client, number=1):
        job_id = self.submit(client, number)
        self.ready(client, job_id)
        result = self.drain(client, job_id)
        self.assertEqual(client.result("asset.jobs.take", {"job_id": job_id}), result)
        return result

    def query(self, client, number=1):
        return client.result("asset.cache.query", self.inputs(client, number))

    def inspect(self, client, key):
        return client.result("asset.cache.inspect", {
            "key": key, "expected_revision": client.result("asset.cache.status")["revision"]})

    def test_identity_catalog_and_cache_keep_dirty_scene_history(self):
        source = b"Shark harbor\n"
        with self.client() as client:
            importers = client.result("asset.importers")["importers"]
            self.assertEqual({row["id"] for row in importers},
                             {"shark.bytes", "shark.gltf", "shark.gltf-resource"})
            self.assertTrue(all(row["version"] == row["payload_version"] == 1 for row in importers))
            mutation = client.result("scene.mutation_status")
            edited = client.result("scene.mutate", {
                "mutation_id": mutation["next"], "expected_revision": mutation["revision"],
                "operation": {"type": "command", "command": {"type": "set_name",
                    "entity_id": pipes.ENTITIES[0], "name": "Unsaved through imports"}}})
            self.assertTrue(edited["outcome"]["ok"])
            self.assertTrue(edited["outcome"]["value"]["result"]["ok"])
            dirty = client.result("project.status")
            history = client.result("scene.history")
            mutations = client.result("scene.mutation_status")
            self.assertTrue(dirty["dirty"])
            self.assertEqual(history["undo_entries"], 1)

            metadata = self.add_asset(client, 1, "harbor.txt", source,
                                      settings={"transform": "uppercase-ascii"})
            self.add_asset(client, 2, "missing.txt", None)
            read = client.result("asset.metadata.read", {"path": self.sidecars[0]})
            self.assertEqual(read["metadata"], metadata)
            self.assertEqual(client.result("asset.metadata.release", {"revision": read["revision"]}),
                             {"released": True})
            catalog = self.refresh(client)
            self.assertEqual(self.refresh(client), catalog)
            first = client.result("asset.catalog.query", {
                "expected_revision": catalog, "page_size": 1})
            self.assertEqual((first["total"], first["next_offset"]), (2, 1))
            self.assertEqual(first["records"][0]["metadata"], metadata)
            self.assertEqual(first["records"][0]["source"], "assets/harbor.txt")
            second = client.result("asset.catalog.query", {
                "expected_revision": catalog, "offset": first["next_offset"], "page_size": 1})
            self.assertFalse(second["records"][0]["source_present"])
            self.assertIsNone(second["next_offset"])
            for number, availability in ((1, "source_present"), (2, "source_missing"), (3, "unknown_id")):
                resolved = client.result("asset.catalog.resolve", {
                    "asset_id": asset_id(number), "expected_revision": catalog})
                self.assertEqual(resolved["availability"], availability)
                self.assertEqual(resolved["record"] is None, number == 3)
            missing = self.query(client)
            self.assertEqual(missing["state"], "missing")
            self.assertIsNone(missing["last_valid_key"])
            self.assertIsNone(missing["blocked_by"])
            job_id = self.submit(client)
            self.ready(client, job_id)
            self.assertFalse(self.cache_path.exists(), "Observing ready work must not publish it")
            outcome = self.drain(client, job_id)
            self.assertEqual(client.result("asset.jobs.cancel", {"job_id": job_id}), outcome)
            self.assertEqual(client.result("asset.jobs.take", {"job_id": job_id}), outcome)
            key = outcome["summary"]["root_key"]
            self.assertEqual(key, missing["required_key"])
            self.assertEqual((outcome["summary"]["nodes"], outcome["summary"]["imported"],
                              outcome["summary"]["reused"]), (1, 1, 0))
            artifact = self.inspect(client, key)
            # Independent expected bytes for the versioned byte payload envelope.
            payload = (b"SHKBYT01" + (1).to_bytes(4, "little") + uuid.UUID(asset_id(1)).bytes
                       + len(source).to_bytes(8, "little") + source.upper())
            self.assertEqual(artifact["payload_bytes"], len(payload))
            self.assertEqual(artifact["payload_digest"], hashlib.sha256(payload).hexdigest())
            self.assertEqual(artifact["source_digest"], hashlib.sha256(source).hexdigest())
            self.assertEqual(artifact["dependencies"], [])
            self.assertNotIn("payload", artifact)
            self.assertEqual(self.query(client)["state"], "fresh")
            usage = client.result("asset.cache.status")
            self.assertEqual((usage["artifacts"], usage["bindings"], usage["payload_bytes"]),
                             (1, 1, len(payload)))
            self.assertFalse(usage["unpublished_stage"])
            self.assertEqual(client.result("asset.cache.reload", {"expected_revision": usage["revision"]}),
                             {"revision": usage["revision"]})
            pruned = client.result("asset.cache.prune", {"expected_revision": usage["revision"]})
            self.assertFalse(pruned["changed"])
            self.assertIsNone(pruned["publication"])
            self.assertEqual(client.result("asset.cache.recover"), {"removed": 0})
            cleared = client.result("asset.cache.clear", {"expected_revision": usage["revision"]})
            self.assertTrue(cleared["changed"])
            self.publication(cleared["publication"])
            self.assertEqual(self.query(client)["state"], "missing")
            rebuilt = self.import_asset(client)
            self.assertEqual(rebuilt["summary"]["root_key"], key)
            self.assertEqual(self.inspect(client, key), artifact)
            self.assertEqual(client.result("project.status"), dirty)
            self.assertEqual(client.result("scene.history"), history)
            self.assertEqual(client.result("scene.mutation_status"), mutations)
            client.finish()
        with self.client() as client:
            self.refresh(client)
            self.assertEqual(self.query(client)["last_valid_key"], key)
            self.assertEqual(self.inspect(client, key), artifact)
            self.assertFalse(client.result("project.status")["dirty"])
            client.finish()

    def test_pending_dedup_four_retained_slots_cancel_take_and_close(self):
        with self.client() as client:
            for number in range(1, 6):
                self.add_asset(client, number, str(number) + ".txt", bytes([64 + number]))
            self.refresh(client)
            first = self.submit(client)
            self.ready(client, first)
            duplicate = client.result("asset.jobs.submit", self.inputs(client))
            self.assertEqual(duplicate, {"job_id": first, "deduplicated": True})
            pending = [first] + [self.submit(client, number) for number in range(2, 5)]
            self.assertEqual(len(client.result("asset.jobs.list")["jobs"]), 4)
            self.rpc_error(client.call("asset.jobs.submit", self.inputs(client, 5)), "conflict")
            self.rpc_error(client.call("asset.jobs.take", {"job_id": first}), "conflict")
            foreign = {**first, "scope": asset_id(99)}
            self.rpc_error(client.call("asset.jobs.cancel", {"job_id": foreign}), "conflict")
            cancelled = client.result("asset.jobs.cancel", {"job_id": pending.pop()})
            self.assertEqual(cancelled["state"], "cancelled")
            self.assertEqual(client.result("asset.jobs.take", {"job_id": cancelled["job_id"]}), cancelled)
            replacement = self.submit(client, 5)
            self.assertGreater(int(replacement["sequence"]), int(cancelled["job_id"]["sequence"]))
            pending.append(replacement)
            client.result("asset.jobs.cancel", {"job_id": first})
            drained = self.drain(client, first, "cancelled")
            self.assertIsNone(drained["error"])
            self.assertEqual(client.result("asset.jobs.take", {"job_id": first}), drained)
            self.rpc_error(client.call("asset.jobs.take", {"job_id": first}))
            closed = client.result("asset.jobs.close")
            self.assertTrue(closed["closed"])
            self.assertEqual({row["state"] for row in closed["jobs"]}, {"cancelled"})
            self.assertEqual(len(closed["jobs"]), 3)
            self.assertEqual(client.result("asset.jobs.close"), closed)
            for row in closed["jobs"]:
                self.assertEqual(client.result("asset.jobs.take", {"job_id": row["job_id"]}), row)
            self.assertEqual(client.result("asset.jobs.list"), {"jobs": []})
            self.rpc_error(client.call("asset.jobs.submit", self.inputs(client)), "conflict")
            self.assertEqual(client.result("asset.cache.status")["artifacts"], 0)
            self.assertFalse(self.cache_path.exists())
            client.finish()

    def test_stale_source_metadata_catalog_and_project_fences_keep_last_valid(self):
        with self.client() as client:
            self.add_asset(client, 1, "harbor.txt", b"published")
            self.refresh(client)
            key = self.import_asset(client)["summary"]["root_key"]
            cached_bytes = self.cache_path.read_bytes()
            source = self.root / "assets/harbor.txt"
            sidecar = self.root / self.sidecars[0]
            source.write_bytes(b"candidate")
            self.assertEqual(self.query(client)["state"], "stale")

            def rejected_after_ready(change):
                job_id = self.submit(client)
                self.ready(client, job_id)
                change()
                failed = self.drain(client, job_id, "failed")
                self.assertEqual(failed["error"]["code"], "conflict", failed)
                self.assertTrue(failed["error"]["context"]["operation"])
                self.assertEqual(client.result("asset.jobs.take", {"job_id": job_id}), failed)
                self.assertEqual(self.cache_path.read_bytes(), cached_bytes)
                self.assertEqual(self.query(client)["last_valid_key"], key)

            rejected_after_ready(lambda: source.write_bytes(b"edited after capture"))
            rejected_after_ready(lambda: sidecar.write_bytes(sidecar.read_bytes() + b"\n"))
            self.refresh(client)
            rejected_after_ready(lambda: self.refresh(client, []))
            self.refresh(client)
            job_id = self.submit(client)
            self.ready(client, job_id)
            project = self.root / "FeatureWorld.sharkproject"
            try:
                project.write_bytes(self.original["FeatureWorld.sharkproject"] + b"\n")
                self.rpc_error(client.call("asset.jobs.drain"), "conflict")
                self.assertEqual(client.result("asset.jobs.list")["jobs"][0]["state"], "ready")
                client.result("asset.jobs.cancel", {"job_id": job_id})
                closed = client.result("asset.jobs.close")
                self.assertEqual(closed["jobs"][0]["state"], "cancelled")
                self.assertEqual(client.result("asset.jobs.take", {"job_id": job_id}), closed["jobs"][0])
                self.assertEqual(self.cache_path.read_bytes(), cached_bytes)
            finally:
                project.write_bytes(self.original["FeatureWorld.sharkproject"])
            client.finish()

    def test_real_gltf_dependency_import_malformed_update_and_server_privacy(self):
        fixture_manifest = json.loads((FIXTURES / "manifest.json").read_text(encoding="utf-8"))
        with self.client() as client:
            for name in ("external.gltf", "triangle.bin"):
                data = (FIXTURES / name).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), fixture_manifest[name]["sha256"])
                self.assertEqual(len(data), fixture_manifest[name]["bytes"])
            source = (FIXTURES / "external.gltf").read_bytes()
            resource = (FIXTURES / "triangle.bin").read_bytes()
            self.add_asset(client, 1, "triangle.gltf", source, kind="gltf", audience="client",
                           settings={"path": "models/triangle.gltf", "resources": [
                               {"path": "models/triangle.bin", "asset": asset_id(2)}]},
                           dependencies=[asset_id(2)])
            self.add_asset(client, 2, "triangle.bin", resource, kind="gltf-resource")
            self.add_asset(client, 3, "private-rules.txt", b"server rules", audience="server")
            self.refresh(client)
            success = self.import_asset(client)
            self.assertEqual((success["summary"]["nodes"], success["summary"]["imported"]), (2, 2))
            key = success["summary"]["root_key"]
            artifact = self.inspect(client, key)
            dependency_key = self.query(client, 2)["last_valid_key"]
            self.assertEqual(artifact["kind"], "gltf")
            self.assertEqual(artifact["importer"], "shark.gltf")
            self.assertEqual(artifact["source_digest"], fixture_manifest["external.gltf"]["sha256"])
            self.assertEqual(artifact["dependencies"], [{"asset_id": asset_id(2), "key": dependency_key}])
            dependency = self.inspect(client, dependency_key)
            self.assertEqual(dependency["importer"], "shark.gltf-resource")
            self.assertEqual(dependency["source_digest"], fixture_manifest["triangle.bin"]["sha256"])
            self.assertEqual(client.result("asset.cache.status")["artifacts"], 2)
            cached_bytes = self.cache_path.read_bytes()
            blocked = self.query(client, 3)
            self.assertEqual(blocked["state"], "missing")
            self.assertIsNone(blocked["last_valid_key"])
            self.assertEqual(blocked["blocked_by"]["context"]["asset_id"], asset_id(3))
            self.rpc_error(client.call("asset.jobs.submit", self.inputs(client, 3)),
                           blocked["blocked_by"]["code"])
            (self.root / "assets/triangle.gltf").write_bytes(b"{invalid glTF")
            job_id = self.submit(client)
            self.ready(client, job_id)
            failed = self.drain(client, job_id, "failed")
            self.assertTrue(failed["error"]["message"])
            self.assertTrue(failed["error"]["context"]["operation"])
            self.assertEqual(client.result("asset.jobs.take", {"job_id": job_id}), failed)
            self.assertEqual(self.query(client)["last_valid_key"], key)
            self.assertEqual(self.inspect(client, key), artifact)
            self.assertEqual(self.cache_path.read_bytes(), cached_bytes)
            client.finish()

    def test_metadata_conflicts_guards_and_eof_or_kill_release_owner(self):
        with self.client() as client:
            metadata = self.add_asset(client, 1, "harbor.txt", b"unpublished")
            path = self.sidecars[0]
            first = client.result("asset.metadata.read", {"path": path})
            self.rpc_error(client.call("asset.metadata.write", {
                "path": path, "metadata": metadata, "expected_revision": None}), "conflict")
            self.rpc_error(client.call("asset.metadata.write", {
                "path": "assets/other.txt.sharkasset", "metadata": metadata,
                "expected_revision": first["revision"]}), "conflict")
            (self.root / path).write_bytes((self.root / path).read_bytes() + b"\n")
            self.rpc_error(client.call("asset.metadata.write", {
                "path": path, "metadata": metadata, "expected_revision": first["revision"]}), "conflict")
            current = client.result("asset.metadata.read", {"path": path})
            changed = {**metadata, "extras": {"note": "exact native revision"}}
            written = client.result("asset.metadata.write", {
                "path": path, "metadata": changed, "expected_revision": current["revision"]})
            self.publication(written["publication"])
            client.result("asset.metadata.release", {"revision": written["revision"]})
            self.rpc_error(client.call("asset.metadata.write", {
                "path": path, "metadata": metadata, "expected_revision": written["revision"]}), "conflict")
            for invalid in ("../outside.sharkasset", "FeatureWorld.sharkproject.sharkasset",
                            "scenes/FeatureWorld.sharkscene.sharkasset"):
                with self.subTest(path=invalid):
                    self.rpc_error(client.call("asset.metadata.write", {
                        "path": invalid, "metadata": metadata, "expected_revision": None}))
            catalog = self.refresh(client)
            self.rpc_error(client.call("text.read", {"path": path}))
            self.rpc_error(client.call("asset.catalog.query", {
                "expected_revision": {**catalog, "value": 1}}), "invalid_argument")
            self.rpc_error(client.call("asset.cache.status", {"unexpected": True}), "invalid_argument")
            duplicate = "assets/duplicate.txt.sharkasset"
            (self.root / duplicate).write_bytes((self.root / path).read_bytes())
            self.rpc_error(client.call("asset.catalog.refresh", {
                "sidecars": [path, duplicate], "expected_revision": catalog}))
            self.assertEqual(client.result("asset.catalog.status")["revision"], catalog)
            self.assertEqual(client.result("asset.catalog.query", {"expected_revision": catalog})["total"], 1)
            with self.client(opened=False) as contender:
                self.rpc_error(contender.call("project.open", {"path": "FeatureWorld.sharkproject"}), "conflict")
                contender.finish()
            old_job = self.submit(client)
            self.ready(client, old_job)
            client.finish()  # EOF cancels the ready job before releasing the project writer.
        self.assertFalse(self.cache_path.exists())
        with self.client() as client:
            self.rpc_error(client.call("asset.jobs.cancel", {"job_id": old_job}), "conflict")
            self.refresh(client)
            job_id = self.submit(client)
            self.assertNotEqual(job_id["scope"], old_job["scope"])
            self.ready(client, job_id)
            client.process.kill()
            client.process.wait(timeout=pipes.TIMEOUT)
        self.assertFalse(self.cache_path.exists(), "Process death must not publish undrained work")
        with self.client() as client:
            self.assertEqual(client.result("asset.cache.status")["artifacts"], 0)
            self.assertEqual(client.result("asset.cache.recover"), {"removed": 0})
            client.finish()

    def test_metadata_token_limit_and_repair_before_lazy_cache_admission(self):
        with self.client() as client:
            metadata = self.add_asset(client, 1, "harbor.txt", b"repairable source")
            path = self.sidecars[0]
            # The initial write retains one token; these reads fill the other 31.
            tokens = [client.result("asset.metadata.read", {"path": path})["revision"]
                      for _ in range(31)]
            self.rpc_error(client.call("asset.metadata.read", {"path": path}), "conflict")
            changed = {**metadata, "extras": {"note": "replacement at capacity"}}
            replaced = client.result("asset.metadata.write", {
                "path": path, "metadata": changed, "expected_revision": tokens[0]})
            self.publication(replaced["publication"])
            self.rpc_error(client.call("asset.metadata.release", {"revision": tokens[0]}), "conflict")
            self.rpc_error(client.call("asset.metadata.read", {"path": path}), "conflict")
            client.result("asset.metadata.release", {"revision": replaced["revision"]})
            read = client.result("asset.metadata.read", {"path": path})
            self.assertEqual(read["metadata"], changed)
            client.result("asset.metadata.release", {"revision": read["revision"]})

            corrupt = b"not a development cache"
            self.cache_path.write_bytes(corrupt)
            self.rpc_error(client.call("asset.cache.status"))
            read = client.result("asset.metadata.read", {"path": path})
            self.assertEqual(read["metadata"], changed)
            client.result("asset.metadata.release", {"revision": read["revision"]})
            catalog = self.refresh(client)
            page = client.result("asset.catalog.query", {"expected_revision": catalog})
            self.assertEqual(page["records"][0]["metadata"], changed)
            stage = self.root / (".shark-cache-stage-" + asset_id(99))
            stage.write_bytes(b"interrupted unpublished fixture")
            self.assertEqual(client.result("asset.cache.recover"), {"removed": 1})
            self.assertFalse(stage.exists())
            self.assertEqual(self.cache_path.read_bytes(), corrupt)
            # Closing before a cache owner exists must survive its later opening.
            self.assertEqual(client.result("asset.jobs.close"), {"closed": True, "jobs": []})
            self.cache_path.unlink()  # Explicit repair of this test's disposable corrupt cache.
            inputs = self.inputs(client)
            self.assertEqual(client.result("asset.cache.status")["artifacts"], 0)
            self.rpc_error(client.call("asset.jobs.submit", inputs), "conflict")
            self.assertEqual(client.result("asset.jobs.list"), {"jobs": []})
            self.assertFalse(self.cache_path.exists())
            client.finish()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--author", type=Path, required=True)
    parser.add_argument("--project", type=Path,
                        default=pipes.ROOT / "assets/FeatureWorld/FeatureWorld.sharkproject")
    args, remaining = parser.parse_known_args()
    args.author, args.project = args.author.resolve(), args.project.resolve()
    pipes.ARGS = args
    unittest.main(argv=[sys.argv[0], *remaining])
