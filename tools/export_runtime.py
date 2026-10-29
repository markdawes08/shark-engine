"""Bounded live-owner coordination for the early Windows scene exporter.

The native CPU worker never spawns descendants. It keeps ProjectFiles locked
until the existing package publisher finishes its final validation and rename.
This module is build tooling and is never shipped with Runtime.
"""
from __future__ import annotations

from contextlib import contextmanager
import json
import os
from pathlib import Path
import platform
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

from graphics_probe import finite_float, reject_constant, unique_object


MAX_MESSAGE = 2 * 1024 * 1024
WORKER_TIMEOUT = 60.0


class ExportError(ValueError):
    def __init__(self, code: str, message: str, **context):
        super().__init__(message)
        self.detail = {"code": code, "message": message, **context}


def require_host(target: str) -> None:
    if (os.name != "nt" or platform.machine().lower() not in {"amd64", "x86_64"}
            or struct.calcsize("P") != 8 or target != "windows-x86_64"):
        raise ExportError("capability_unavailable",
                          "Early export requires native Windows x64 and target windows-x86_64")


class ExportSession:
    """Three bounded response lines; one native child, no shell or descendants."""

    def __init__(self, command: list[str], *, cwd: Path, timeout: float = WORKER_TIMEOUT):
        if not 0 < timeout <= WORKER_TIMEOUT:
            raise ValueError("Export session timeout must be in (0, 60] seconds")
        self.command, self.cwd, self.timeout = command, cwd, timeout
        self.process = None
        self.stderr = None
        self.reader = None
        self.responses = queue.Queue(maxsize=4)
        self.overflow = threading.Event()
        self.released = False

    def __enter__(self):
        self.deadline = time.monotonic() + self.timeout
        self.stderr = tempfile.TemporaryFile()
        try:
            self.process = subprocess.Popen(self.command, cwd=self.cwd, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=self.stderr)
            self.reader = threading.Thread(target=self._read, daemon=True)
            self.reader.start()
            self.content = self.response("READY")["content"]
            return self
        except BaseException:
            self.close()
            raise

    def _read(self):
        try:
            for _ in range(4):
                line = self.process.stdout.readline(MAX_MESSAGE + 1)
                self.responses.put_nowait(line)
                if not line or len(line) > MAX_MESSAGE:
                    return
            # More than the three successful messages is a protocol failure.
            self.overflow.set()
        except (OSError, ValueError, queue.Full):
            self.overflow.set()

    def remaining(self) -> float:
        value = self.deadline - time.monotonic()
        if value <= 0:
            raise ExportError("timeout", "Export content worker exceeded its 60-second deadline")
        return value

    def response(self, expected: str) -> dict:
        try:
            line = self.responses.get(timeout=self.remaining())
        except queue.Empty as error:
            raise ExportError("timeout", "Export content worker response deadline expired") from error
        if self.overflow.is_set() or not line or len(line) > MAX_MESSAGE or not line.endswith(b"\n"):
            raise ExportError("execution_failed", "Missing, oversized or excessive export worker response")
        try:
            row = json.loads(line.decode("utf-8"), object_pairs_hook=unique_object,
                             parse_constant=reject_constant, parse_float=finite_float)
        except (UnicodeError, ValueError) as error:
            raise ExportError("execution_failed", "Invalid export worker JSON") from error
        if (not isinstance(row, dict) or row.get("format") != "shark.export-content-worker"
                or type(row.get("schema_version")) is not int or row["schema_version"] != 1
                or row.get("renderer_initialized") is not False):
            raise ExportError("execution_failed", "Unsupported export worker response")
        if row.get("status") == "ERROR":
            detail = row.get("error")
            if not isinstance(detail, dict):
                raise ExportError("execution_failed", "Export worker failed without an error record")
            raise ExportError(str(detail.get("code", "execution_failed")),
                              str(detail.get("message", "Export worker failed")), worker=detail)
        fields = {"format", "schema_version", "status", "renderer_initialized"}
        if expected == "READY":
            fields.add("content")
        if set(row) != fields or row.get("status") != expected:
            raise ExportError("execution_failed", "Unexpected export worker protocol state")
        return row

    def send(self, command: bytes) -> None:
        self.remaining()
        if self.process.poll() is not None:
            raise ExportError("execution_failed", "Export worker exited before publication completed")
        try:
            self.process.stdin.write(command)
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise ExportError("execution_failed", "Export worker input closed") from error

    def validate(self) -> None:
        self.send(b"VALIDATE\n")
        self.response("VALIDATED")
        if self.process.poll() is not None:
            raise ExportError("execution_failed", "Export owner ended before package publication")

    def release(self) -> None:
        self.send(b"RELEASE\n")
        self.response("RELEASED")
        try:
            result = self.process.wait(timeout=self.remaining())
        except subprocess.TimeoutExpired as error:
            raise ExportError("timeout", "Export worker did not release its project owner") from error
        self.reader.join(timeout=min(1.0, self.remaining()))
        if (result != 0 or self.reader.is_alive() or self.overflow.is_set()
                or os.fstat(self.stderr.fileno()).st_size):
            raise ExportError("execution_failed", "Export worker failed during release")
        # After the RELEASED line, only EOF may follow.
        try:
            if self.responses.get_nowait() != b"":
                raise ExportError("execution_failed", "Unexpected trailing export worker output")
        except queue.Empty as error:
            raise ExportError("execution_failed", "Export worker output did not close") from error
        self.released = True

    def close(self) -> None:
        if self.process is not None:
            if self.process.poll() is None:
                self.process.kill()
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired as error:
                raise ExportError("timeout", "Export worker did not exit after termination") from error
            if self.reader is not None:
                self.reader.join(timeout=1)
            self.process.stdin.close()
            self.process.stdout.close()
        if self.stderr is not None:
            self.stderr.close()

    def __exit__(self, *_):
        self.close()


@contextmanager
def content_scratch(parent: Path):
    # Only the specific fresh directory returned by mkdtemp is eligible for cleanup.
    import package_runtime as package
    package.assert_no_links(parent)
    path = Path(tempfile.mkdtemp(prefix=".runtime-export-", dir=parent)).resolve()
    try:
        yield path
    finally:
        if path.exists():
            package.assert_no_links(path)
            if path.parent != parent.resolve() or not path.name.startswith(".runtime-export-"):
                raise ExportError("execution_failed", "Refusing cleanup outside export scratch parent")
            for entry in path.rglob("*"):
                package.assert_no_links(entry)
            shutil.rmtree(path)


def export_package(args) -> dict:
    import package_runtime as package
    report = {"format": "shark.client-export", "schema_version": 1, "status": "ERROR",
              "published": False, "qualification": "NOT_RUN", "renderer_initialized": False,
              "platform": "windows-x86_64", "artifacts": []}
    try:
        require_host(args.target)
        output, project = args.output.absolute(), args.project.absolute()
        package.assert_no_links(output)
        package.assert_no_links(project)
        if output.exists():
            raise ExportError("conflict", "Package output must be fresh; existing output was preserved")
        if output.resolve().is_relative_to(project.parent.resolve()):
            raise ExportError("invalid_argument", "Export output must be outside the source project root")
        if len(args.sidecar) > 4096:
            raise ExportError("invalid_argument", "Sidecar inventory exceeds the catalog limit")
        exporter = args.exporter.absolute()
        package.regular_file(exporter)
        # Collect slow toolchain/binary inputs before acquiring the project owner.
        payload, manifest = package.collect_runtime_payload(args.build_dir, args.crt_dir)
        vertex, pixel, provenance = package.scene_shader_inputs(args.shaders)
        notices, notice_inputs = {}, {}
        if len(args.content_notice) > 128:
            raise ExportError("invalid_argument", "Content notice inventory exceeds its bound")
        for value in args.content_notice:
            license_id, separator, source = value.partition("=")
            if not separator or not source or license_id in notices:
                raise ExportError("invalid_argument", "Use one LICENSE=PATH notice per license identifier")
            package.content_text(license_id, 128)
            path = Path(source).absolute()
            data = package.regular_file(path, maximum=64 * 1024)
            notices[license_id] = data
            notice_inputs[path] = data
        if sum(map(len, notices.values())) > 1024 * 1024:
            raise ExportError("invalid_argument", "Content notices exceed the combined byte bound")
        output.parent.mkdir(parents=True, exist_ok=True)
        with content_scratch(output.parent) as scratch:
            content = scratch / "content"
            command = [str(exporter), "--project", str(project), "--camera", args.camera,
                       "--content-target", args.content_target, "--output", str(content),
                       "--vertex-shader", str(vertex), "--pixel-shader", str(pixel)]
            for sidecar in args.sidecar:
                command.extend(["--sidecar", sidecar])
            with ExportSession(command, cwd=scratch) as session:
                payload, manifest = package.assemble_scene_package(
                    payload, manifest, content, session.content, args.shaders,
                    expected_generation=provenance["generation"], content_notices=notices)
                def validate():
                    for path, data in notice_inputs.items():
                        if package.regular_file(path, maximum=64 * 1024) != data:
                            raise ExportError("conflict", "Content notice changed before publication", path=str(path))
                    session.validate()
                published = package.publish(output, payload, manifest, before_publish=validate)
                # Rename success is publication even if subsequent worker/cleanup reporting fails.
                report.update(status="PASS", published=True, package_directory=str(output),
                              manifest_sha256=package.digest(package.json_bytes(published)),
                              files=len(published["files"]), content=session.content)
                report["artifacts"] = [{"path": "package-manifest.json",
                                        "sha256": report["manifest_sha256"]}]
                session.release()
        return report
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        report["status"] = "FAIL" if report["published"] else "ERROR"
        report["error"] = (error.detail if isinstance(error, ExportError) else
                           {"code": "execution_failed", "message": str(error)})
        return report


def authoring_export(request_file: Path) -> int:
    """Private inverted exchange: caller owns ProjectFiles and captured content.

    The native caller owns the process-tree deadline. No source project is opened
    here, and no independent ExportContent child can acquire a second writer.
    """
    import package_runtime as package

    def emit(value):
        raw = json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("utf-8") + b"\n"
        if len(raw) > 1024 * 1024:
            raise ExportError("execution_failed", "Authoring export response exceeds its bound")
        sys.stdout.buffer.write(raw)
        sys.stdout.buffer.flush()

    def receive(expected):
        if sys.stdin.buffer.readline(17) != expected + b"\n":
            raise ExportError("conflict", "Authoring owner closed or rejected package publication")

    report = {"format": "shark.client-export", "schema_version": 1, "status": "ERROR",
              "published": False, "qualification": "NOT_RUN", "renderer_initialized": False,
              "platform": "windows-x86_64", "artifacts": []}
    try:
        require_host("windows-x86_64")
        request = json.loads(package.regular_file(request_file, maximum=1024 * 1024),
                             object_pairs_hook=unique_object, parse_constant=reject_constant,
                             parse_float=finite_float)
        package.exact_record(request, {"format", "schema_version", "project_root", "build_dir",
                                      "crt_dir", "shaders", "output", "content_directory",
                                      "content_record", "notices"}, "authoring export request")
        if request["format"] != "shark.authoring-export-request" or type(request["schema_version"]) is not int or request["schema_version"] != 1:
            raise ExportError("invalid_argument", "Unsupported authoring export request")
        paths = {}
        for name in ("project_root", "build_dir", "crt_dir", "shaders", "output",
                     "content_directory", "content_record"):
            paths[name] = Path(package.content_text(request[name], 4096)).absolute()
            package.assert_no_links(paths[name])
        output = paths["output"]
        if output.exists():
            raise ExportError("conflict", "Package output must be fresh; existing output was preserved")
        if output.resolve().is_relative_to(paths["project_root"].resolve()):
            raise ExportError("invalid_argument", "Export output must be outside the source project root")
        payload, manifest = package.collect_runtime_payload(paths["build_dir"], paths["crt_dir"])
        vertex, pixel, provenance = package.scene_shader_inputs(paths["shaders"])
        if not isinstance(request["notices"], list) or len(request["notices"]) > 128:
            raise ExportError("invalid_argument", "Content notices exceed their inventory bound")
        notices, notice_inputs = {}, {}
        for row in request["notices"]:
            package.exact_record(row, {"license", "path"}, "content notice")
            license_id = package.content_text(row["license"], 128)
            if license_id in notices:
                raise ExportError("invalid_argument", "Content notice license is duplicated")
            path = Path(package.content_text(row["path"], 4096)).absolute()
            data = package.regular_file(path, maximum=64 * 1024)
            notices[license_id], notice_inputs[path] = data, data
        if sum(map(len, notices.values())) > 1024 * 1024:
            raise ExportError("invalid_argument", "Content notices exceed their combined byte bound")
        emit({"format": "shark.authoring-export-helper", "schema_version": 1,
              "status": "INPUTS", "vertex": str(vertex), "pixel": str(pixel)})
        receive(b"ASSEMBLE")
        content = json.loads(package.regular_file(paths["content_record"], maximum=1024 * 1024),
                             object_pairs_hook=unique_object, parse_constant=reject_constant,
                             parse_float=finite_float)
        payload, manifest = package.assemble_scene_package(
            payload, manifest, paths["content_directory"], content, paths["shaders"],
            expected_generation=provenance["generation"], content_notices=notices)

        def validate():
            emit({"format": "shark.authoring-export-helper", "schema_version": 1,
                  "status": "READY_TO_PUBLISH"})
            receive(b"PUBLISH")
            for path, data in notice_inputs.items():
                if package.regular_file(path, maximum=64 * 1024) != data:
                    raise ExportError("conflict", "Content notice changed before publication", path=str(path))

        # Keep child staging inside the native caller's owned scratch tree. It
        # can remove uncommitted bytes even if this process is terminated.
        published = package.publish(output, payload, manifest, before_publish=validate,
                                    staging_parent=request_file.absolute().parent)
        report.update(status="PASS", published=True, package_directory=str(output),
                      manifest_sha256=package.digest(package.json_bytes(published)),
                      files=len(published["files"]), content=content)
        report["artifacts"] = [{"path": "package-manifest.json", "sha256": report["manifest_sha256"]}]
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        report["status"] = "FAIL" if report["published"] else "ERROR"
        report["error"] = (error.detail if isinstance(error, ExportError) else
                           {"code": "execution_failed", "message": str(error)})
    emit(report)
    return 0 if report["status"] == "PASS" else (1 if report["published"] else 2)
