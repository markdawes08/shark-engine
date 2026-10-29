"""Qualify retained-owner capture and export through real terminal processes."""
from __future__ import annotations

import argparse
from dataclasses import asdict
import json
from pathlib import Path
import re
import sys
import time

import test_authoring_protocol_process as pipes

sys.path.insert(0, str(pipes.ROOT / "tools"))
import offscreen_smoke as raster
import runtime_scene_smoke
import scene_render_smoke
from graphics_probe import (check_layer_process, layer_environment, sha256,
                            validation_tools)
from process_runner import run_process

require = raster.require
CAMERA = pipes.ENTITIES[0]
CUBE = pipes.ENTITIES[3]
WIDTH, HEIGHT = 160, 120


def rejected(row, code):
    require("error" in row and row["error"]["data"]["code"] == code,
            f"Expected {code}, received {row}")
    require(bool(row["error"]["message"]), "Rejected operation has no diagnostic")


def edit(client, command):
    current = client.result("scene.mutation_status")
    result = client.result("scene.mutate", {
        "mutation_id": current["next"], "expected_revision": current["revision"],
        "operation": {"type": "command", "command": command}})
    require(result["outcome"]["ok"] and result["outcome"]["value"]["result"]["ok"],
            f"Shared mutation failed: {result}")


def preserve(client, before, history, mutations, original):
    require(client.result("project.status") == before, "Artifact operation changed active scene state")
    require(client.result("scene.history") == history, "Artifact operation changed shared history")
    require(client.result("scene.mutation_status") == mutations, "Artifact operation changed retry retention")
    require(all(path.read_bytes() == data for path, data in original.items()),
            "Artifact operation changed saved source bytes")


def capture_check(report, directory, device, revision):
    require(report.get("format") == "shark.authoring-capture" and report.get("schema_version") == 1,
            "Unsupported terminal capture report")
    require(report.get("published") is True and report.get("succeeded") is True and
            report.get("source_dirty") is True and report.get("closed") is True,
            "Capture lacks dirty-source publication and clean close")
    require(report.get("source_revision") == revision and
            report.get("rendered_revision", {}).get("scene_instance") != revision["scene_instance"],
            "Capture did not distinguish source and reconstructed scene identity")
    require((report.get("project_id"), report.get("scene_id"), report.get("camera_id")) ==
            (pipes.PROJECT, pipes.SCENE, CAMERA), "Capture identities differ from retained owner")
    require((report.get("width"), report.get("height"), report.get("color_space")) ==
            (WIDTH, HEIGHT, "linear") and report.get("subpixel_precision_bits") == 8,
            "Capture changed extent, encoding or fixed current-PC subpixel profile")
    require(report.get("draw_count") == report.get("mesh_instances") == 1 and
            report.get("hidden") == report.get("culled") == 0, "Unexpected capture extraction counts")
    require(report.get("validation") == "required" and report.get("adapter", {}).get("uuid") == device,
            "Capture did not use the explicitly selected validated GPU")
    adapter = report["adapter"]
    require(all(isinstance(adapter.get(key), str) and adapter[key]
                for key in ("name", "driver_name", "driver_info")) and
            type(adapter.get("driver_version")) is int, "Capture lacks actual device/driver identity")
    require(report.get("diagnostics") == {"warnings": 0, "errors": 0, "dropped": 0},
            "Capture graphics diagnostics are not clean")
    gpu, viewport = report["gpu"], report["viewport"]
    require(type(gpu["submitted"]) is int and gpu["submitted"] > 0 and
            gpu["submitted"] == gpu["completed"] and gpu["created_resources"] > 0 and
            gpu["created_resources"] == gpu["retired_resources"], "Capture GPU work remains incomplete")
    require(viewport["generation"] > 0 and viewport["retired_generations"] == viewport["generation"] and
            viewport["rendered_frames"] == 1 and viewport["live_target_resources"] == 0 and
            viewport["pending"] is False, "Capture viewport owners did not retire")
    require(Path(report["output"]).resolve() == directory.resolve(), "Capture report names another output")
    require(report.get("color") == "color.rgba" and report.get("depth") == "depth.f32" and
            set(report["artifacts"]) == {"color.rgba", "depth.f32"}, "Unexpected capture file format")
    require({path.name for path in directory.iterdir()} == {"color.rgba", "depth.f32", "report.json"},
            "Capture has an unexpected artifact inventory")
    require(json.loads((directory / "report.json").read_text(encoding="utf-8")) == report,
            "Returned capture report differs from its published file")
    for name in ("color.rgba", "depth.f32"):
        path = directory / name
        require(report["artifacts"][name] == {"bytes": WIDTH * HEIGHT * 4, "sha256": sha256(path)} and
                path.stat().st_size == WIDTH * HEIGHT * 4, "Capture file differs from its bounded identity")
    comparison = raster.compare_readback((directory / "color.rgba").read_bytes(),
                                         (directory / "depth.f32").read_bytes(),
                                         runtime_scene_smoke.reference("featureworld"))
    require(comparison["status"] == "PASS", f"Capture failed the independent fixed oracle: {comparison}")
    return comparison


def run(args, summary):
    output = args.output
    lock = sha256(pipes.ROOT / "dependencies.lock.json")
    shaders, vertex, pixel = scene_render_smoke.shader_inputs(args.shaders)
    validation = validation_tools(args.validation_manifest, lock)
    summary["process_security"] = check_layer_process()
    summary["inputs"] = {"author_sha256": sha256(args.author), "dependency_lock_sha256": lock,
                         "shader_generation": shaders["generation"],
                         "validation_manifest_sha256": sha256(args.validation_manifest)}
    settings = output / "vk_layer_settings.txt"
    settings.write_text("# Required core/synchronization and NVRHI validation; no overrides.\n", encoding="utf-8")
    implicit = output / "implicit-layers-empty"
    implicit.mkdir()
    environment, summary["layer_environment"] = layer_environment(validation, settings, implicit)
    project = output / "project"
    (project / "scenes").mkdir(parents=True)
    original = {}
    for relative in ("FeatureWorld.sharkproject", "scenes/FeatureWorld.sharkscene"):
        path = project / relative
        original[path] = (pipes.ROOT / "assets/FeatureWorld" / relative).read_bytes()
        path.write_bytes(original[path])

    for index, device in enumerate(args.device):
        row = {"device": device, "status": "FAIL"}
        summary["captures"].append(row)
        options = ["--capture-vertex-shader", str(vertex), "--capture-pixel-shader", str(pixel),
                   "--capture-device", device, "--capture-validation", "required",
                   "--export-python", str(args.python), "--export-tool", str(args.export_tool)]
        with pipes.PipeClient(output, project, "FeatureWorld.sharkproject", options=options,
                              timeout=90, env=environment) as client:
            try:
                discovery = client.result("protocol.discover")
                methods = {entry["name"] for entry in discovery["methods"]}
                require({"scene.run", "scene.capture", "scene.export"} <= methods and
                        discovery["capabilities"]["device_capture"] is True and
                        discovery["capabilities"]["standalone_export"] is True,
                        "Configured native adapters are missing from discovery")
                camera = client.result("scene.inspect", {
                    "entity_id": CAMERA, "fields": ["camera"]})["entities"][0]["camera"]
                edit(client, {"type": "set_transform", "entity_id": CAMERA,
                              "transform": {"translation": [0, 0, 3], "rotation": [0, 0, 0, 1],
                                            "scale": [1, 1, 1]}})
                edit(client, {"type": "set_camera", "entity_id": CAMERA,
                              "camera": {**camera, "projection": "orthographic",
                                         "orthographic_height": 3, "near_clip": 1, "far_clip": 9}})
                edit(client, {"type": "set_transform", "entity_id": CUBE,
                              "transform": {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1],
                                            "scale": [1, 1, 1]}})
                before = client.result("project.status")
                history = client.result("scene.history")
                mutations = client.result("scene.mutation_status")
                require(before["dirty"] is True and history["undo_entries"] == 3,
                        "Fixture did not retain its three unsaved shared edits")
                content = {"camera_id": CAMERA, "target": {"id": "development", "side": "client"},
                           "expected_revision": before["revision"],
                           "expected_catalog": client.result("asset.catalog.status")["revision"],
                           "expected_cache": client.result("asset.cache.status")["revision"]}
                directory = output / f"capture-{index}"
                parameters = {**content, "output": str(directory), "width": WIDTH, "height": HEIGHT}
                stale = {**before["revision"], "revision": str(int(before["revision"]["revision"]) + 1)}
                rejected(client.call("scene.capture", {**parameters, "expected_revision": stale}), "conflict")
                require(not directory.exists(), "Stale capture published an output")
                unsafe = project / "unsafe-capture"
                rejected(client.call("scene.capture", {**parameters, "output": str(unsafe)}), "path_outside_root")
                require(not unsafe.exists(), "Capture wrote inside the source project")
                missing_parent = output / "absent-parent" / "capture"
                rejected(client.call("scene.capture", {**parameters, "output": str(missing_parent)}), "invalid_path")
                preserve(client, before, history, mutations, original)
                report = client.result("scene.capture", parameters)
                row["report"] = report
                row["comparison"] = capture_check(report, directory, device, before["revision"])
                existing_digest = sha256(directory / "report.json")
                rejected(client.call("scene.capture", parameters), "conflict")
                require(sha256(directory / "report.json") == existing_digest,
                        "Existing capture output was overwritten")
                preserve(client, before, history, mutations, original)

                if index == 0:
                    package = output / "package"
                    export_request = {**content, "build_dir": str(args.runtime_build),
                                      "crt_dir": str(args.crt_dir), "shaders": str(args.shaders),
                                      "output": str(package), "notices": []}
                    rejected(client.call("scene.export", {**export_request, "expected_revision": stale}), "conflict")
                    require(not package.exists(), "Stale export published an output")
                    rejected(client.call("scene.export", {**export_request, "output": str(project / "unsafe-export")}),
                             "path_outside_root")
                    require(not (project / "unsafe-export").exists(), "Export wrote inside the source project")
                    exported = client.result("scene.export", export_request)
                    summary["export"] = exported
                    require(exported["status"] == "PASS" and exported["published"] is True and
                            exported["renderer_initialized"] is False and exported["qualification"] == "NOT_RUN",
                            f"Export did not complete CPU publication: {exported}")
                    content_record = exported["content"]
                    require(content_record["dirty"] is True and content_record["project"] == pipes.PROJECT and
                            content_record["scene"] == pipes.SCENE and content_record["camera"] == CAMERA and
                            content_record["target"] == "development" and content_record["sources"] == [],
                            "Export did not consume the current dirty builtin scene")
                    require(content_record["files"]["bin/content/startup.sharkscene"]["sha256"] == report["scene_digest"],
                            "Export and capture consumed different prepared scenes")
                    require(sha256(package / "package-manifest.json") == exported["manifest_sha256"],
                            "Published package manifest digest differs")
                    inspected = run_process([str(package / "bin/Runtime.exe"), "--check-package",
                                             str(package / "bin/startup.sharkstart")],
                                            cwd=output, timeout=15)
                    require(inspected.returncode == 0 and not inspected.timed_out and
                            not inspected.cleanup_errors and not inspected.stderr,
                            f"Packaged CPU inspection failed: {asdict(inspected)}")
                    inspection = json.loads(inspected.stdout)
                    require(inspection["format"] == "shark.package-inspection" and inspection["schema_version"] == 1 and
                            inspection["status"] == "PASS" and inspection["renderer_initialized"] is False and
                            inspection["qualification"] == "NOT_RUN",
                            "Packaged CPU inspection did not pass without graphics")
                    require((inspection["project"], inspection["scene"], inspection["camera"],
                             inspection["target"], inspection["mesh_count"]) ==
                            (pipes.PROJECT, pipes.SCENE, CAMERA, "windows-x86_64", 0) and
                            inspection["startup_sha256"] == content_record["files"]["bin/startup.sharkstart"]["sha256"],
                            "Packaged CPU inspection differs from the terminal export candidate")
                    summary["package_inspection"] = inspection
                    rejected(client.call("scene.export", export_request), "conflict")
                    require(sha256(package / "package-manifest.json") == exported["manifest_sha256"],
                            "Existing package was overwritten")
                    preserve(client, before, history, mutations, original)

                # Export/capture must retain the writer, not silently reopen saved state.
                with pipes.PipeClient(output, project, timeout=10) as contender:
                    rejected(contender.call("project.open", {"path": "FeatureWorld.sharkproject"}), "conflict")
                    contender.finish()
                rejected(client.call("project.close"), "conflict")
                preserve(client, before, history, mutations, original)
                client.result("project.close", {"unsaved": "discard", "expected_revision": before["revision"]})
                client.finish()
            except Exception:
                if client.process.poll() is None:
                    client.process.kill()
                    client.process.wait(timeout=5)
                client.stderr.seek(0)
                diagnostic = client.stderr.read(1024 * 1024 + 1)
                if diagnostic:
                    (output / f"capture-{index}.stderr.log").write_bytes(diagnostic)
                raise
        with pipes.PipeClient(output, project, "FeatureWorld.sharkproject", timeout=10) as reopened:
            require(reopened.result("project.status")["dirty"] is False,
                    "Explicit close did not hand off the saved project")
            reopened.finish()
        require(all(path.read_bytes() == data for path, data in original.items()),
                "Closing the artifact owner wrote unsaved changes")
        row["status"] = "PASS"
    summary["status"] = "PASS"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("author", "shaders", "validation-manifest", "python", "export-tool", "runtime-build", "crt-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--device", action="append", required=True)
    args = parser.parse_args()
    for name in ("author", "shaders", "validation_manifest", "python", "export_tool", "runtime_build", "crt_dir", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if len(args.device) != 2 or len(set(args.device)) != 2 or not all(re.fullmatch(r"[0-9a-f]{32}", value) for value in args.device):
        parser.error("Supply the two distinct explicit current-PC GPU UUIDs")
    if args.output.exists():
        parser.error("Evidence output must be a fresh directory")
    args.output.mkdir(parents=True)
    pipes.ARGS = args
    summary = {"checkpoint": "15d", "status": "FAIL", "full_requirement_15": "NOT_RUN",
               "captures": [], "errors": []}
    started = time.monotonic()
    try:
        run(args, summary)
    except Exception as error:
        summary["errors"].append(str(error))
    summary["elapsed_seconds"] = round(time.monotonic() - started, 3)
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "summary": str(args.output / "summary.json"),
                      "errors": summary["errors"]}))
    return int(summary["status"] != "PASS")


if __name__ == "__main__":
    raise SystemExit(main())
