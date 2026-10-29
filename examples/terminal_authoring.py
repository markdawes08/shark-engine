"""Create a Shark project through public pipes; optionally capture and export it.

Run from any directory with an explicit --author and fresh --output. The default
licensed offline triangle is copied into the new project; original fixtures are
never edited. This example has no engine Python binding, third-party package,
editor automation or test-module dependency. See docs/authoring-protocol.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
IDS = {name: "15e00000-0000-4000-8000-" + f"{number:012x}"
       for number, name in enumerate(("project", "world", "scene", "camera", "entity",
                                      "mesh", "resource", "batch"), 1)}
PROJECT_PATH = "Terminal.sharkproject"
SCENE_PATH = "scenes/Main.sharkscene"
SIDECARS = ["models/triangle.gltf.sharkasset", "models/triangle.bin.sharkasset"]
TARGET = {"id": "terminal-example", "side": "client"}
FIELDS = ["name", "transform", "hierarchy", "camera", "mesh_renderer"]
REQUEST_LIMIT, RESPONSE_LIMIT, STDERR_LIMIT = 65536, 1048576, 65536


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError("Nonfinite JSON value: " + value)


def bounded_bytes(path, maximum):
    require(path.stat().st_size <= maximum, f"File exceeds {maximum} bytes: {path}")
    with path.open("rb") as source:
        data = source.read(maximum + 1)
    require(len(data) <= maximum, f"File grew beyond its byte bound: {path}")
    return data


def digest(path):
    data = bounded_bytes(path, 4 * 1024 * 1024)
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class Author:
    """One owner, one request at a time, bounded reads/writes and native cleanup."""

    def __init__(self, executable, project_root, options):
        self.process = subprocess.Popen(
            [str(executable), "--stdio", "--root", str(project_root), *options],
            cwd=project_root, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.sequence = 0
        self.diagnostics = bytearray()
        self.diagnostic_overflow = False
        self.exchange_worker = None
        self.stderr_reader = threading.Thread(target=self._stderr, daemon=True)
        self.stderr_reader.start()

    def _stderr(self):
        try:
            while block := self.process.stderr.read(4096):
                remaining = STDERR_LIMIT - len(self.diagnostics)
                self.diagnostics.extend(block[:remaining])
                if len(block) > remaining:
                    self.diagnostic_overflow = True
                    self.process.kill()
                    return
        except (OSError, ValueError):
            return

    def call(self, method, params=None, *, timeout=15):
        self.sequence += 1
        identity = self.sequence
        data = (json.dumps({"jsonrpc": "2.0", "protocol_version": 1, "id": identity,
                            "method": method, "params": params or {}}, ensure_ascii=False,
                           allow_nan=False, separators=(",", ":")) + "\n").encode("utf-8")
        require(len(data) - 1 <= REQUEST_LIMIT, "Request exceeds the protocol byte limit")
        completed = queue.Queue(maxsize=1)

        def exchange():
            try:
                self.process.stdin.write(data)
                self.process.stdin.flush()
                completed.put(self.process.stdout.readline(RESPONSE_LIMIT + 2))
            except (OSError, ValueError) as error:
                completed.put(error)

        worker = threading.Thread(target=exchange, daemon=True)
        self.exchange_worker = worker
        worker.start()
        try:
            line = completed.get(timeout=timeout)
        except queue.Empty as error:
            self.abort()
            worker.join(timeout=5)
            raise TimeoutError(f"{method}: response deadline expired; reconcile before retry") from error
        worker.join(timeout=5)
        if isinstance(line, Exception):
            raise RuntimeError(f"{method}: pipe failed; reconcile before retry: {line}")
        require(line and line.endswith(b"\n") and not line.endswith(b"\r\n") and
                len(line) <= RESPONSE_LIMIT + 1, f"{method}: missing or oversized protocol response")
        reply = json.loads(line.decode("utf-8"), object_pairs_hook=unique, parse_constant=reject_constant)
        require(isinstance(reply, dict) and reply.get("jsonrpc") == "2.0" and
                type(reply.get("protocol_version")) is int and reply["protocol_version"] == 1 and
                type(reply.get("id")) is int and reply["id"] == identity and
                set(reply) in ({"jsonrpc", "protocol_version", "id", "result"},
                               {"jsonrpc", "protocol_version", "id", "error"}),
                f"{method}: response envelope or correlation differs")
        if "error" in reply:
            raise RuntimeError(f"{method}: {json.dumps(reply['error'], ensure_ascii=False)}")
        require(not self.diagnostic_overflow, "Author stderr exceeded its bound")
        return reply["result"]

    def abort(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        try:
            if exception_type is None:
                try:
                    self.process.stdin.close()
                    code = self.process.wait(timeout=10)
                except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                    self.abort()
                    raise RuntimeError("Author did not close cleanly after EOF") from error
                self.stderr_reader.join(timeout=5)
                require(code == 0 and not self.diagnostic_overflow,
                        f"Author failed ({code}): {self.diagnostics.decode('utf-8', errors='replace')}")
            else:
                self.abort()
        finally:
            if self.process.poll() is None:
                self.abort()
            if self.exchange_worker is not None:
                self.exchange_worker.join(timeout=5)
            self.stderr_reader.join(timeout=5)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                try:
                    stream.close()
                except (OSError, ValueError):
                    pass


def publication(receipt):
    require(receipt is not None and receipt.get("published") is True and receipt.get("bytes", 0) > 0,
            f"File was not published: {receipt}")
    require(receipt.get("durability") in ("file_flushed", "file_and_directory_flushed",
                                         "directory_flush_failed"), "Missing publication durability")


def mutate(author, operation):
    status = author.call("scene.mutation_status")
    result = author.call("scene.mutate", {"mutation_id": status["next"],
                        "expected_revision": status["revision"], "operation": operation})
    require(result["outcome"]["ok"], f"Mutation outcome unavailable: {result}")
    require(result["outcome"]["value"]["result"]["ok"], f"Mutation rejected: {result}")
    return result


def metadata(asset, kind, settings, dependencies):
    return {"format": "shark.asset", "schema_version": 1, "project_id": IDS["project"],
            "asset_id": IDS[asset], "kind": kind, "importer": "shark." + kind,
            "origin": "Shark original offline triangle; copied and edited by terminal example",
            "license": "MIT", "audience": "client", "settings": settings,
            "dependencies": dependencies}


def refresh(author):
    revision = author.call("asset.catalog.status")["revision"]
    return author.call("asset.catalog.refresh", {"expected_revision": revision, "sidecars": SIDECARS})


def import_triangle(author):
    request = {"asset_id": IDS["mesh"], "target": TARGET,
               "expected_catalog": author.call("asset.catalog.status")["revision"],
               "expected_cache": author.call("asset.cache.status")["revision"]}
    job = author.call("asset.jobs.submit", request)["job_id"]
    deadline = time.monotonic() + 15
    while True:
        rows = author.call("asset.jobs.drain")["jobs"]
        row = next(value for value in rows if value["job_id"] == job)
        if row["state"] in ("succeeded", "failed", "cancelled"):
            break
        if time.monotonic() >= deadline:
            author.call("asset.jobs.cancel", {"job_id": job})
            author.call("asset.jobs.close")
            raise TimeoutError("Import deadline expired; inspect cache/outcome before retry")
    require(row["state"] == "succeeded" and row["error"] is None and row["summary"] is not None
            and row["commit"] is not None, f"Import did not succeed: {row}")
    if row["commit"]["changed"]:
        publication(row["commit"]["publication"])
    require(author.call("asset.jobs.take", {"job_id": job}) == row, "Import acknowledgement differs")
    descriptor = author.call("asset.cache.inspect", {"key": row["summary"]["root_key"],
                             "expected_revision": author.call("asset.cache.status")["revision"]})
    return {"outcome": row, "descriptor": descriptor}


def entities(author):
    # This example has two entities. Explicit fields include optional components.
    page = author.call("scene.query", {"fields": FIELDS, "page_size": 16})
    require(page["complete"] and page["next_cursor"] is None, "Example entity query is incomplete")
    return sorted(page["entities"], key=lambda entity: entity["entity_id"])


def snapshot(author, scene_path):
    return {"status": author.call("project.status"), "entities": entities(author),
            "history": author.call("scene.history"), "scene_file": digest(scene_path)}


def save(author):
    result = author.call("scene.save", {"expected_revision": author.call("project.status")["revision"]})
    require(result["success"] and result["current_scene_clean"], f"Save did not complete: {result}")
    publication(result["publication"])
    return result


def run_example(args, summary):
    project = args.output / "project"
    for directory in ("scenes", "models", "licenses"):
        (project / directory).mkdir(parents=True)
    # Ingesting external source bytes is ordinary filesystem work. Typed project,
    # scene and asset metadata writes below go through the retained public owner.
    for source, destination in (("external.gltf", "models/triangle.gltf"),
                                ("triangle.bin", "models/triangle.bin"),
                                ("LICENSE", "licenses/fixture-MIT.txt")):
        value = bounded_bytes(args.fixtures / source, 1024 * 1024)
        (project / destination).write_bytes(value)
    scene_path = project / SCENE_PATH
    summary.update(ids=IDS, project_root=str(project), project_path=PROJECT_PATH,
                   sources_initial={name: digest(project / name) for name in
                                    ("models/triangle.gltf", "models/triangle.bin", "licenses/fixture-MIT.txt")})
    options = []
    if args.capture_device:
        options += ["--capture-vertex-shader", str(args.capture_vertex_shader),
                    "--capture-pixel-shader", str(args.capture_pixel_shader),
                    "--capture-device", args.capture_device, "--capture-validation", args.capture_validation]
    if args.export_tool:
        options += ["--export-python", str(args.export_python), "--export-tool", str(args.export_tool)]
    with Author(args.author, project, options) as author:
        discovery = author.call("protocol.discover")
        summary["capabilities"] = discovery["capabilities"]
        require(discovery["capabilities"].get("cpu_authoring") and
                discovery["capabilities"].get("scene_runs"), "Host lacks CPU authoring/run capabilities")
        if args.capture_device:
            require(discovery["capabilities"].get("device_capture"), "Host lacks configured capture")
        if args.export_tool:
            require(discovery["capabilities"].get("standalone_export"), "Host lacks configured export")
        created = author.call("project.create", {"path": PROJECT_PATH, "scene_path": SCENE_PATH,
                              "name": "Terminal harbor", "project_id": IDS["project"],
                              "world_id": IDS["world"], "scene_id": IDS["scene"]})
        require(created["success"], f"Project creation failed: {created}")
        for receipt in created["published"].values():
            publication(receipt)
        settings = {"path": "models/triangle.gltf", "resources": [
                    {"path": "models/triangle.bin", "asset": IDS["resource"]}]}
        documents = [metadata("mesh", "gltf", settings, [IDS["resource"]]),
                     metadata("resource", "gltf-resource", {}, [])]
        for path, document in zip(SIDECARS, documents):
            result = author.call("asset.metadata.write", {"path": path, "expected_revision": None,
                                 "metadata": document})
            publication(result["publication"])
            author.call("asset.metadata.release", {"revision": result["revision"]})
        refresh(author)
        summary["imports"] = {"initial": import_triangle(author)}

        # The first job has been taken. Deliberately edit copied source and its
        # settings, refresh the complete catalog, then import the new exact input.
        source_path = project / "models/triangle.gltf"
        source = json.loads(bounded_bytes(source_path, 1024 * 1024).decode("utf-8"))
        source["asset"]["generator"] = "Shark terminal example edited source"
        source_path.write_bytes((json.dumps(source, separators=(",", ":")) + "\n").encode("utf-8"))
        observed = author.call("asset.metadata.read", {"path": SIDECARS[0]})
        documents[0]["settings"] = {**settings, "scene": 0}
        changed = author.call("asset.metadata.write", {"path": SIDECARS[0], "metadata": documents[0],
                              "expected_revision": observed["revision"]})
        publication(changed["publication"])
        author.call("asset.metadata.release", {"revision": changed["revision"]})
        refresh(author)
        summary["imports"]["modified"] = import_triangle(author)
        require(summary["imports"]["initial"]["outcome"]["summary"]["root_key"] !=
                summary["imports"]["modified"]["outcome"]["summary"]["root_key"],
                "Edited asset did not produce a distinct import key")
        summary["sources_modified"] = {name: digest(project / name) for name in
                                       ("models/triangle.gltf", "models/triangle.bin", *SIDECARS)}

        batch = {"id": IDS["batch"], "commands": [
            {"type": "create_entity", "entity_id": IDS["camera"], "name": "Terminal camera",
             "transform": {"translation": [0, 0, 3], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
             "camera": {"projection": "orthographic", "vertical_fov_radians": math.pi / 3,
                        "orthographic_height": 3, "near_clip": 1, "far_clip": 9}},
            {"type": "create_entity", "entity_id": IDS["entity"], "name": "Imported triangle",
             "transform": {"translation": [-1, -1, 0], "rotation": [0, 0, 0, 1], "scale": [2, 2, 2]},
             "mesh_renderer": {"asset_id": IDS["mesh"], "mesh_index": 0, "visible": True}}]}
        preview = author.call("scene.preview", {"batch": batch,
                              "expected_revision": author.call("project.status")["revision"]})
        require(preview["outcome"]["ok"], f"Scene preview rejected: {preview}")
        mutate(author, {"type": "batch", "batch": batch})
        summary["first_save"] = save(author)
        summary["saved"] = snapshot(author, scene_path)
        author.call("project.close")

    # A new process must reopen the actual published source and reacquire the lock.
    with Author(args.author, project, options) as author:
        author.call("project.open", {"path": PROJECT_PATH})
        refresh(author)
        summary["reopened"] = snapshot(author, scene_path)
        require(summary["reopened"]["entities"] == summary["saved"]["entities"] and
                not summary["reopened"]["status"]["dirty"], "Saved semantic scene did not survive reopen")
        mutate(author, {"type": "command", "command": {"type": "set_name", "entity_id": IDS["entity"],
                                                        "name": "Unsaved triangle \u96ea"}})
        before = {"status": author.call("project.status"), "history": author.call("scene.history"),
                  "mutations": author.call("scene.mutation_status"), "scene_file": digest(scene_path)}
        summary["prior_artifacts"] = before
        summary["run"] = author.call("scene.run", {"expected_revision": before["status"]["revision"],
                                     "seed": "1337", "ticks": 3, "expected_entities": 2,
                                     "run_id": "terminal-example", "revision": "authored-example"})
        require(summary["run"]["report"]["status"] == "PASS", "Static scene integrity run failed")
        content = {"expected_revision": before["status"]["revision"], "camera_id": IDS["camera"],
                   "target": TARGET, "expected_catalog": author.call("asset.catalog.status")["revision"],
                   "expected_cache": author.call("asset.cache.status")["revision"]}
        summary["capture"] = summary["export"] = None
        if args.capture_device:
            captured = author.call("scene.capture", {**content, "output": str(args.output / "capture"),
                                   "width": 160, "height": 120}, timeout=70)
            summary["capture"] = captured
            require(captured["published"] and captured["succeeded"], f"Capture failed: {captured}")
        if args.export_tool:
            exported = author.call("scene.export", {**content, "output": str(args.output / "package"),
                                  "build_dir": str(args.runtime_build), "crt_dir": str(args.crt_dir),
                                  "shaders": str(args.shaders), "notices": [
                                      {"license": "MIT", "path": str(project / "licenses/fixture-MIT.txt")}]},
                                   timeout=70)
            summary["export"] = exported
            require(exported["published"] and exported["status"] == "PASS", f"Export failed: {exported}")
        after = {"status": author.call("project.status"), "history": author.call("scene.history"),
                 "mutations": author.call("scene.mutation_status"), "scene_file": digest(scene_path)}
        summary["after_artifacts"] = after
        require(after == before, "Run/capture/export changed current authoring state or saved source")
        summary["final_save"] = save(author)
        summary["final_entities"] = entities(author)
        author.call("project.close")

    # The retained project is ready for a human or another terminal owner.
    with Author(args.author, project, []) as author:
        author.call("project.open", {"path": PROJECT_PATH})
        summary["handoff"] = snapshot(author, scene_path)
        require(not summary["handoff"]["status"]["dirty"] and
                summary["handoff"]["entities"] == summary["final_entities"], "Final owner handoff differs")
        author.call("project.close")
    summary["status"] = "PASS"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", type=Path, required=True, help="Built SharkAuthor or SharkAuthorClient executable")
    parser.add_argument("--output", type=Path, required=True, help="Fresh directory; project and artifacts are retained here")
    parser.add_argument("--fixtures", type=Path, default=ROOT / "tests/fixtures/gltf_source",
                        help="Directory containing the licensed offline external.gltf, triangle.bin and LICENSE")
    for name in ("capture-vertex-shader", "capture-pixel-shader", "export-python", "export-tool",
                 "runtime-build", "crt-dir", "shaders"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--capture-device", help="Exact GPU UUID; requires both shader flags and a Client host")
    parser.add_argument("--capture-validation", choices=("required", "off"), default="required")
    args = parser.parse_args()
    capture = [args.capture_device, args.capture_vertex_shader, args.capture_pixel_shader]
    exporting = [args.export_python, args.export_tool, args.runtime_build, args.crt_dir, args.shaders]
    if any(capture) and not all(capture):
        parser.error("Capture requires --capture-device and both capture shader paths")
    if any(exporting) and not all(exporting):
        parser.error("Export requires --export-python, --export-tool, --runtime-build, --crt-dir and --shaders")
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    if args.output.exists() or not args.output.parent.is_dir():
        parser.error("--output must be absent and its parent must already exist")
    args.output.mkdir()
    summary = {"format": "shark.terminal-example", "schema_version": 1, "status": "FAIL", "errors": []}
    started = time.monotonic()
    try:
        run_example(args, summary)
    except Exception as error:
        summary["errors"].append(str(error))
    summary["elapsed_seconds"] = round(time.monotonic() - started, 3)
    path = args.output / "summary.json"
    path.write_text(json.dumps(summary, ensure_ascii=False, allow_nan=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "summary": str(path), "errors": summary["errors"]}))
    return int(summary["status"] != "PASS")


if __name__ == "__main__":
    raise SystemExit(main())
