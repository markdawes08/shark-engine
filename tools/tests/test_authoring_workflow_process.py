"""Connected public terminal example, independent saved state and owner handoff."""
from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

import test_authoring_protocol_process as pipes


IDS = {name: f"15e00000-0000-4000-8000-{index:012x}" for index, name in enumerate(
    ("project", "world", "scene", "camera", "entity", "mesh", "resource", "batch"), 1)}
PROJECT_PATH = "Terminal.sharkproject"
SCENE_PATH = "scenes/Main.sharkscene"
SIDECARS = ["models/triangle.gltf.sharkasset", "models/triangle.bin.sharkasset"]
TARGET = {"id": "terminal-example", "side": "client"}
CAMERA = {"projection": "orthographic", "vertical_fov_radians": math.pi / 3,
          "orthographic_height": 3, "near_clip": 1, "far_clip": 9}
TRANSFORMS = {
    IDS["camera"]: {"translation": [0, 0, 3], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
    IDS["entity"]: {"translation": [-1, -1, 0], "rotation": [0, 0, 0, 1], "scale": [2, 2, 2]},
}
MESH = {"asset_id": IDS["mesh"], "mesh_index": 0, "visible": True}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_json(path):
    require(path.stat().st_size <= 1024 * 1024, f"Unbounded workflow JSON: {path}")
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=pipes.unique)


def digest(path):
    data = path.read_bytes()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def check_entities(rows, triangle_name):
    require(len(rows) == 2 and {row["entity_id"] for row in rows} ==
            {IDS["camera"], IDS["entity"]}, "Connected scene must contain the two authored entities")
    for row in rows:
        camera = row["entity_id"] == IDS["camera"]
        require(row["name"] == ("Terminal camera" if camera else triangle_name), "Authored entity name differs")
        require(row["transform"]["local"] == TRANSFORMS[row["entity_id"]], "Authored transform differs")
        require(row["camera"] == (CAMERA if camera else None), "Authored Camera differs")
        require(row["mesh_renderer"] == (None if camera else MESH), "Authored MeshRenderer differs")


def check_workflow(report, output: Path, *, artifacts=frozenset()):
    """Shared CPU semantics; the artifact fixture separately checks actual GPUs/packages."""
    require(set(artifacts) <= {"capture", "export"}, "Unknown expected workflow artifact")
    require((report.get("format"), report.get("schema_version"), report.get("status"), report.get("errors")) ==
            ("shark.terminal-example", 1, "PASS", []), f"Connected example did not pass: {report}")
    require(report["ids"] == IDS, "Example identity inventory differs")
    project = output / "project"
    require(Path(report["project_root"]).resolve() == project.resolve() and
            report["project_path"] == PROJECT_PATH, "Example project paths differ")
    document = read_json(project / PROJECT_PATH)
    require((document["format"], document["schema_version"], document["project_id"], document["world_id"],
             document["name"], document["startup_scene"], document["scenes"]) ==
            ("shark.project", 1, IDS["project"], IDS["world"], "Terminal harbor", IDS["scene"],
             [{"scene_id": IDS["scene"], "path": SCENE_PATH}]), "Saved project differs from authored intent")
    scene = read_json(project / SCENE_PATH)
    require((scene["format"], scene["schema_version"], scene["project_id"], scene["scene_id"]) ==
            ("shark.scene", 1, IDS["project"], IDS["scene"]), "Saved scene identity differs")
    require(len(scene["entities"]) == 2 and {row["entity_id"] for row in scene["entities"]} ==
            {IDS["camera"], IDS["entity"]}, "Saved scene contains unexpected entities")
    for row in scene["entities"]:
        camera = row["entity_id"] == IDS["camera"]
        expected = {"shark.name": {"schema_version": 1,
                                  "value": "Terminal camera" if camera else "Unsaved triangle \u96ea"},
                    "shark.transform": {"schema_version": 1, **TRANSFORMS[row["entity_id"]]}}
        expected["shark.camera" if camera else "shark.mesh_renderer"] = {
            "schema_version": 1, **(CAMERA if camera else MESH)}
        require(row["components"] == expected and row.get("parent") is None,
                "Saved entity components differ from independent expected values")

    fixtures = pipes.ROOT / "tests/fixtures/gltf_source"
    manifest = read_json(fixtures / "manifest.json")
    initial = report["sources_initial"]
    require(initial["models/triangle.gltf"] == manifest["external.gltf"] and
            initial["models/triangle.bin"] == manifest["triangle.bin"], "Initial licensed fixture identity differs")
    require((project / "licenses/fixture-MIT.txt").read_bytes() == (fixtures / "LICENSE").read_bytes(),
            "Connected project lost the complete fixture notice")
    require(initial["licenses/fixture-MIT.txt"] == digest(fixtures / "LICENSE"), "Initial license identity differs")
    require((project / "models/triangle.bin").read_bytes() == (fixtures / "triangle.bin").read_bytes(),
            "Copied triangle resource differs from the offline geometry")
    source = read_json(fixtures / "external.gltf")
    source["asset"]["generator"] = "Shark terminal example edited source"
    require(read_json(project / "models/triangle.gltf") == source,
            "Modified source changed more than the declared generator field")
    modified = report["sources_modified"]
    require(set(modified) == {"models/triangle.gltf", "models/triangle.bin", *SIDECARS},
            "Modified source inventory differs")
    for name, record in modified.items():
        require(record == digest(project / name), f"Modified input digest differs: {name}")
    require(modified["models/triangle.gltf"] != initial["models/triangle.gltf"] and
            modified["models/triangle.bin"] == initial["models/triangle.bin"], "Source edit was not isolated")
    root_metadata, resource_metadata = (read_json(project / name) for name in SIDECARS)
    for metadata, name, kind in ((root_metadata, "mesh", "gltf"),
                                  (resource_metadata, "resource", "gltf-resource")):
        require((metadata["format"], metadata["schema_version"], metadata["project_id"], metadata["asset_id"],
                 metadata["kind"], metadata["importer"], metadata["license"], metadata["audience"]) ==
                ("shark.asset", 1, IDS["project"], IDS[name], kind, "shark." + kind, "MIT", "client"),
                "Typed import metadata identity or audience differs")
    require(root_metadata["dependencies"] == [IDS["resource"]] and root_metadata["settings"] == {
                "path": "models/triangle.gltf", "scene": 0,
                "resources": [{"path": "models/triangle.bin", "asset": IDS["resource"]}]},
            "Modified root import settings/dependencies differ")
    require(resource_metadata["dependencies"] == [] and resource_metadata["settings"] == {},
            "Resource importer metadata differs")
    imports = report["imports"]
    for name, imported, reused, source_digest in (
            ("initial", 2, 0, initial["models/triangle.gltf"]["sha256"]),
            ("modified", 1, 1, modified["models/triangle.gltf"]["sha256"])):
        outcome, descriptor = imports[name]["outcome"], imports[name]["descriptor"]
        require(outcome["state"] == "succeeded" and outcome["error"] is None and
                outcome["completed_nodes"] == outcome["total_nodes"] == 2, "Connected import did not complete")
        summary = outcome["summary"]
        require((summary["root"], summary["target"], summary["nodes"], summary["imported"], summary["reused"]) ==
                (IDS["mesh"], TARGET, 2, imported, reused), "Connected import work differs")
        require(outcome["commit"]["changed"] and outcome["commit"]["publication"]["published"],
                "Connected import lacks actual cache publication")
        require((descriptor["asset_id"], descriptor["kind"], descriptor["importer"], descriptor["target"],
                 descriptor["source_digest"], descriptor["key"]) ==
                (IDS["mesh"], "gltf", "shark.gltf", TARGET, source_digest, summary["root_key"]),
                "Imported descriptor differs from captured inputs")
        require(len(descriptor["dependencies"]) == 1 and
                descriptor["dependencies"][0]["asset_id"] == IDS["resource"], "Missing imported resource closure")
    first, second = imports["initial"]["descriptor"], imports["modified"]["descriptor"]
    require(first["key"] != second["key"] and first["settings_digest"] != second["settings_digest"] and
            first["dependencies"] == second["dependencies"], "Reimport did not change the root and reuse its resource")

    saved, reopened, handoff = report["saved"], report["reopened"], report["handoff"]
    check_entities(saved["entities"], "Imported triangle")
    require(saved["entities"] == reopened["entities"] and saved["scene_file"] == reopened["scene_file"],
            "Saved authored values did not survive actual process reopen")
    for row in (saved, reopened, handoff):
        status = row["status"]
        require(status["open"] and not status["dirty"] and
                (status["project_id"], status["world_id"], status["scene_id"]) ==
                (IDS["project"], IDS["world"], IDS["scene"]), "Saved/reopened owner status differs")
    require(saved["history"]["undo_entries"] == 1 and reopened["history"]["undo_entries"] == 0 and
            handoff["history"]["undo_entries"] == 0, "Save/reopen history semantics differ")
    before = report["prior_artifacts"]
    require(before == report["after_artifacts"] and before["status"]["dirty"] is True and
            before["history"]["undo_entries"] == 1 and before["scene_file"] == saved["scene_file"],
            "Artifact work changed unsaved values, history, retries or saved source")
    final_digest = digest(project / SCENE_PATH)
    require(final_digest == handoff["scene_file"] and final_digest != saved["scene_file"],
            "Explicit final save did not publish the unsaved edit")
    check_entities(report["final_entities"], "Unsaved triangle \u96ea")
    require(handoff["entities"] == report["final_entities"], "Final process handoff lost authored values")
    for name in ("first_save", "final_save"):
        receipt = report[name]
        require(receipt["success"] and receipt["current_scene_clean"] and receipt["publication"]["published"],
                f"Explicit save failed: {name}")
    run = report["run"]
    require(run["observed_revision"] == before["status"]["revision"] and run["dirty"] is True and
            run["entity_count"] == 2 and run["source_digest"] == final_digest["sha256"],
            "Static run did not consume the exact unsaved source values")
    result = run["report"]
    require((result["format"], result["schema_version"], result["status"], result["mode"], result["seed"],
             result["ticks"], result["tick_rate_hz"], result["renderer_initialized"]) ==
            ("shark.scene-run", 1, "PASS", "cpu", 1337, 3, 60, False), "Static run configuration differs")
    require(len(result["cases"]) == 1 and result["cases"][0]["id"] == "scene-integrity" and
            result["cases"][0]["status"] == "PASS" and result["cases"][0]["ticks_completed"] == 3 and
            result["cases"][0]["failure"] is None, "Static integrity case did not complete")
    for name, capability in (("capture", "device_capture"), ("export", "standalone_export")):
        enabled = name in artifacts
        require(report["capabilities"][capability] is enabled and (report[name] is not None) == enabled,
                f"Connected {name} capability/result differs")
        if enabled:
            require(report[name]["published"] is True, f"Connected {name} was not published")
    if "capture" in artifacts:
        require(report["capture"]["source_revision"] == before["status"]["revision"] and
                report["capture"]["source_dirty"] is True, "Capture lost its unsaved owner binding")
    if "export" in artifacts:
        require(report["export"]["status"] == "PASS" and report["export"]["content"]["dirty"] is True,
                "Export lost its unsaved owner binding")


class AuthoringWorkflowProcessTests(unittest.TestCase):
    def test_public_example_connects_import_edit_save_run_and_owner_handoff(self):
        with tempfile.TemporaryDirectory(prefix="shark-terminal-workflow-") as temporary:
            scratch = Path(temporary)
            output = scratch / "caf\u00e9 \u4e16\u754c"
            completed = pipes.run_process([sys.executable, str(pipes.ROOT / "examples/terminal_authoring.py"),
                                          "--author", str(pipes.ARGS.author), "--output", str(output)],
                                         cwd=scratch, timeout=40)
            self.assertEqual(completed.returncode, 0, asdict(completed))
            self.assertFalse(completed.timed_out, asdict(completed))
            self.assertFalse(completed.cleanup_errors, asdict(completed))
            self.assertEqual(completed.stderr, "", asdict(completed))
            completion = json.loads(completed.stdout, object_pairs_hook=pipes.unique)
            self.assertEqual(completion, {"status": "PASS", "summary": str(output / "summary.json"), "errors": []})
            report = read_json(output / "summary.json")
            check_workflow(report, output)
            # Independent next owner consumes the actual files after the example exits.
            with pipes.PipeClient(scratch, output / "project", PROJECT_PATH) as next_owner:
                status = next_owner.result("project.status")
                self.assertFalse(status["dirty"])
                page = next_owner.result("scene.query", {
                    "fields": ["name", "transform", "camera", "mesh_renderer"], "page_size": 16})
                self.assertTrue(page["complete"])
                check_entities(page["entities"], "Unsaved triangle \u96ea")
                next_owner.result("project.close")
                next_owner.finish()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    args.author = args.author.resolve()
    pipes.ARGS = args
    unittest.main(argv=[sys.argv[0], *remaining])
