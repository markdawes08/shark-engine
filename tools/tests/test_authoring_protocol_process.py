"""Actual non-TTY authoring pipes, source ownership and bounded query contracts."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process

ROOT = Path(__file__).resolve().parents[2]
ARGS = None
REQUEST_BYTES = 65536
RESPONSE_BYTES = 1048576
TIMEOUT = 5
PROJECT = "20000000-0000-4000-8000-000000000001"
SCENE = "20000000-0000-4000-8000-000000000003"
ENTITIES = ["20000000-0000-4000-8000-00000000000" + digit for digit in "4567"]
METHODS = {"protocol.discover", "schema.components", "project.open", "project.status",
           "project.close", "scene.query", "scene.inspect", "scene.continue"}


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate response key: " + key)
        result[key] = value
    return result


def request(method, params=None, identity=1):
    return {"jsonrpc": "2.0", "protocol_version": 1, "id": identity,
            "method": method, "params": params or {}}


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def decode(line):
    assert line and len(line) <= RESPONSE_BYTES + 1 and line.endswith(b"\n"), line[:1024]
    assert not line.startswith(b"\xef\xbb\xbf") and not line.endswith(b"\r\n"), line[:1024]
    value = json.loads(line.decode("utf-8"), object_pairs_hook=unique)
    assert isinstance(value, dict) and value.get("jsonrpc") == "2.0", value
    assert type(value.get("protocol_version")) is int and value["protocol_version"] == 1, value
    assert set(value) in ({"jsonrpc", "protocol_version", "id", "result"},
                          {"jsonrpc", "protocol_version", "id", "error"}), value
    return value


class PipeClient:
    """One known native child, bounded response queue and explicit EOF/kill cleanup."""

    def __init__(self, cwd, root=None, project=None):
        command = [str(ARGS.author), "--stdio"]
        if root is not None:
            command += ["--root", str(root)]
        if project is not None:
            command += ["--project", project]
        self.stderr = tempfile.TemporaryFile()
        self.process = subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr)
        self.rows = queue.Queue(maxsize=8)
        self.reader_failure = threading.Event()
        self.writers = []
        self.next_id = 1
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while True:
                line = self.process.stdout.readline(RESPONSE_BYTES + 2)
                self.rows.put_nowait(line)
                if not line or len(line) > RESPONSE_BYTES + 1:
                    return
        except (OSError, ValueError, queue.Full):
            self.reader_failure.set()

    def send(self, data):
        # Ordinary test requests fit the pipe. Large boundary probes use the
        # bounded writer below so a broken transport cannot hang the test parent.
        assert len(data) <= 2048
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def send_large(self, data):
        failure = []
        def write():
            try:
                self.process.stdin.write(data)
                self.process.stdin.flush()
            except (BrokenPipeError, OSError) as error:
                failure.append(error)
        writer = threading.Thread(target=write, daemon=True)
        self.writers.append(writer)
        writer.start()
        return writer, failure

    def row(self):
        try:
            line = self.rows.get(timeout=TIMEOUT)
        except queue.Empty as error:
            raise AssertionError("Authoring response deadline expired") from error
        assert not self.reader_failure.is_set(), "Authoring output exceeded supervisor bounds"
        return decode(line)

    def call(self, method, params=None):
        identity = self.next_id
        self.next_id += 1
        self.send(encoded(request(method, params, identity)) + b"\n")
        row = self.row()
        assert row["id"] == identity, row
        return row

    def result(self, method, params=None):
        row = self.call(method, params)
        assert "result" in row, row
        return row["result"]

    def finish(self, expected=0):
        if not self.process.stdin.closed:
            try:
                self.process.stdin.close()
            except BrokenPipeError:
                assert expected != 0, "Successful host closed its input unexpectedly"
        assert self.process.wait(timeout=TIMEOUT) == expected, self.process.returncode
        self.reader.join(timeout=1)
        assert not self.reader.is_alive() and not self.reader_failure.is_set()
        assert self.rows.get(timeout=1) == b"", "Unexpected trailing authoring output"
        assert self.rows.empty()
        self.stderr.seek(0)
        assert self.stderr.read(4097) == b"", "Protocol requests wrote ordinary/error logs to stderr"

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=1)
        for writer in self.writers:
            writer.join(timeout=1)
            assert not writer.is_alive(), "Input writer survived native child termination"
        self.reader.join(timeout=1)
        assert not self.reader.is_alive(), "Output reader survived native child termination"
        for stream in (self.process.stdin, self.process.stdout, self.stderr):
            if not stream.closed:
                try:
                    stream.close()
                except BrokenPipeError:
                    pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class AuthoringProtocolProcessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shark-authoring-pipes-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / "caf\u00e9 \u4e16\u754c"
        (self.project / "scenes").mkdir(parents=True)
        self.original = {}
        for relative in ("FeatureWorld.sharkproject", "scenes/FeatureWorld.sharkscene"):
            source = ARGS.project.parent / relative
            self.original[relative] = source.read_bytes()
            (self.project / relative).write_bytes(self.original[relative])

    def tearDown(self):
        for relative, expected in self.original.items():
            self.assertEqual((self.project / relative).read_bytes(), expected, relative)

    def client(self, opened=False):
        return PipeClient(self.root, self.project, "FeatureWorld.sharkproject" if opened else None)

    def finite(self, mode, body, *, opened=False, root=True, expected=0):
        command = [str(ARGS.author), mode]
        if root:
            command += ["--root", str(self.project)]
        if opened:
            command += ["--project", "FeatureWorld.sharkproject"]
        process = subprocess.Popen(command, cwd=self.root, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(body, timeout=TIMEOUT)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=1)
        self.assertEqual(process.returncode, expected, (command, stdout, stderr))
        self.assertEqual(stderr, b"", (command, stderr))
        self.assertLessEqual(len(stdout), RESPONSE_BYTES + 1)
        return decode(stdout)

    def error(self, row, code, engine=None):
        self.assertIn("error", row, row)
        error = row["error"]
        self.assertEqual(error["code"], code, row)
        self.assertIsInstance(error["message"], str)
        self.assertTrue(error["message"])
        self.assertEqual(error["data"]["protocol_version"], 1)
        self.assertIsInstance(error["data"]["context"], dict)
        if engine is not None:
            self.assertEqual(error["data"]["code"], engine, row)

    def test_one_shot_discovery_schema_and_no_tty_cli(self):
        row = self.finite("--request", encoded(request("protocol.discover", identity="one")), root=False)
        self.assertEqual(row["id"], "one")
        discovery = row["result"]
        self.assertEqual(discovery["protocol_version"], 1)
        self.assertEqual(discovery["transport"], {"framing": "newline-delimited-json", "batch": False,
                                                 "notifications": True})
        self.assertEqual(discovery["limits"], {"request_bytes": REQUEST_BYTES,
                                              "response_bytes": RESPONSE_BYTES, "retained_cursors": 32})
        self.assertEqual(discovery["capabilities"], {"cpu_authoring": True, "device_capture": False,
                                                    "mutations": False})
        self.assertEqual({method["name"] for method in discovery["methods"]}, METHODS)
        for method in discovery["methods"]:
            self.assertTrue(method["params"] and method["result"] and method["example"], method)
            self.assertEqual(method["execution_context"], "cpu_owner_thread")
            self.assertIs(method["requires_open_project"], method["name"].startswith("scene."))
            self.assertIs(method["requires_project_root"], method["name"] == "project.open")
        schema = self.finite("--request", encoded(request("schema.components")), root=False)["result"]
        self.assertEqual(schema["components"], discovery["components"])
        components = {value["stable_id"]: value for value in schema["components"]}
        self.assertEqual(set(components), {"shark.name", "shark.transform", "shark.camera", "shark.mesh_renderer"})
        self.assertEqual(sum(len(value["properties"]) for value in components.values()), 12)
        for name, value in components.items():
            self.assertEqual(value["version"], 1)
            self.assertEqual(value["required"], name in {"shark.name", "shark.transform"})
            self.assertEqual(value["removable"], name not in {"shark.name", "shark.transform"})
            for prop in value["properties"]:
                self.assertEqual(prop["mutability"], "scene_owner_write")
                self.assertTrue(prop["key"] and prop["type"])
                self.assertIsInstance(prop["constraints"], dict)
                if name == "shark.mesh_renderer" and prop["key"] == "visible":
                    self.assertEqual(prop["type"], "boolean")
                    self.assertIs(prop["default"], True)
                    self.assertEqual(prop["constraints"], {})
                else:
                    self.assertTrue(prop["constraints"])
        for flag in ("--help", "--version"):
            result = run_process([str(ARGS.author), flag], cwd=self.root, timeout=TIMEOUT)
            self.assertEqual(result.returncode, 0, result)
            self.assertFalse(result.timed_out or result.cleanup_errors or result.stderr, result)
            self.assertTrue(result.stdout)
        inspection = self.finite("--request", encoded(request("scene.inspect", {
            "entity_id": ENTITIES[2], "fields": ["name"]})), opened=True)["result"]
        self.assertTrue(inspection["complete"])
        self.assertIsNone(inspection["next_cursor"])
        self.assertEqual(len(inspection["entities"]), 1)
        self.assertEqual(inspection["entities"][0]["name"], "Waypoint \u96ea")

    def test_cli_failures_stay_off_protocol_stdout(self):
        for options in (["--stdio", "--request"],
                        ["--request", "--project", "FeatureWorld.sharkproject"],
                        ["--request", "--root", str(self.project), "--project", "missing.sharkproject"]):
            with self.subTest(options=options):
                process = subprocess.Popen([str(ARGS.author), *options], cwd=self.root,
                                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE)
                try:
                    stdout, stderr = process.communicate(timeout=TIMEOUT)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=1)
                self.assertEqual(process.returncode, 2, (stdout, stderr))
                self.assertEqual(stdout, b"")
                # stderr uses the same structured error envelope; Windows may
                # translate this diagnostic stream's final LF to CRLF.
                row = decode(stderr.replace(b"\r\n", b"\n"))
                self.assertIn("error", row)
                self.assertTrue(row["error"]["message"])

    def test_stdio_fragmented_unicode_notifications_and_clean_eof(self):
        with self.client() as client:
            notification = request("project.open", {"path": "FeatureWorld.sharkproject"})
            del notification["id"]
            client.send(encoded(notification) + b"\n")
            text = encoded(request("project.status", identity="caf\u00e9 \u96ea")) + b"\n"
            split = text.index(b"\xc3") + 1
            client.send(text[:split])
            client.send(text[split:])
            row = client.row()
            self.assertEqual(row["id"], "caf\u00e9 \u96ea")
            self.assertEqual(row["result"]["project_id"], PROJECT)
            self.assertTrue(row["result"]["open"])
            failed_notification = request("unimplemented.method")
            del failed_notification["id"]
            client.send(encoded(failed_notification) + b"\r\n" +
                        encoded(request("project.status", identity=None)) + b"\r\n")
            row = client.row()
            self.assertIsNone(row["id"])
            self.assertFalse(row["result"]["dirty"])
            self.assertEqual(row["result"]["pending_mutations"], 0)
            client.finish()

    def test_queries_projection_filter_pagination_and_revision_fences(self):
        with self.client(opened=True) as client:
            status = client.result("project.status")
            revision = status["revision"]
            self.assertTrue(all(isinstance(value, str) and value.isdecimal() for value in revision.values()))
            inspected = client.result("scene.inspect", {"entity_id": ENTITIES[2],
                "fields": ["name", "transform", "hierarchy"], "expected_revision": revision})
            self.assertEqual(inspected["revision"], revision)
            self.assertEqual(len(inspected["entities"]), 1)
            entity = inspected["entities"][0]
            self.assertEqual(entity["name"], "Waypoint \u96ea")
            # Core counts roots at depth one: Root -> Branch -> Waypoint.
            self.assertEqual(entity["hierarchy"], {"parent_id": ENTITIES[1], "child_count": 0, "depth": 3})
            self.assertEqual(entity["transform"]["local"]["translation"], [3, 0, 0])
            self.assertEqual(entity["transform"]["world_matrix"]["layout"], "column_major")
            self.assertEqual(entity["transform"]["world_matrix"]["values"][12:15], [15, 0, 0])
            roots = client.result("scene.query", {"selection": "roots", "name_prefix": "Root",
                "fields": ["name", "camera", "mesh_renderer"], "page_size": 4, "scan_budget": 4})
            self.assertTrue(roots["complete"])
            self.assertIsNone(roots["next_cursor"])
            rows = {entity["entity_id"]: entity for entity in roots["entities"]}
            self.assertEqual(set(rows), {ENTITIES[0], ENTITIES[3]})
            self.assertEqual(rows[ENTITIES[0]]["camera"]["projection"], "perspective")
            self.assertIsNone(rows[ENTITIES[0]]["mesh_renderer"])
            self.assertIsNone(rows[ENTITIES[3]]["camera"])
            self.assertEqual(rows[ENTITIES[3]]["mesh_renderer"], {
                "asset_id": "00000000-0000-4000-8000-000000000013", "mesh_index": 0, "visible": True})
            self.assertTrue(all("transform" not in item and "hierarchy" not in item for item in rows.values()))
            children = client.result("scene.query", {"selection": "children", "parent_id": ENTITIES[1],
                "name_prefix": "Waypoint \u96ea", "fields": []})
            self.assertEqual([value["entity_id"] for value in children["entities"]], [ENTITIES[2]])
            self.assertEqual(set(children["entities"][0]), {"entity_id", "scope", "authority"})
            page = client.result("scene.query", {"page_size": 1, "scan_budget": 1, "fields": [],
                "expected_revision": revision})
            first_cursor, found = page["next_cursor"], []
            for _ in range(5):
                self.assertEqual(page["revision"], revision)
                self.assertEqual(page["scope"], {"kind": "authored", "scene_id": SCENE})
                self.assertLessEqual(page["scanned_slots"], 1)
                found += [value["entity_id"] for value in page["entities"]]
                if page["complete"]:
                    self.assertIsNone(page["next_cursor"])
                    break
                self.assertIsInstance(page["next_cursor"], str)
                page = client.result("scene.continue", {"cursor": page["next_cursor"]})
            self.assertEqual(sorted(found), ENTITIES)
            self.error(client.call("scene.continue", {"cursor": first_cursor}), -32000, "conflict")
            stale = {**revision, "revision": str(int(revision["revision"]) + 1)}
            self.error(client.call("scene.query", {"expected_revision": stale}), -32000, "conflict")
            outstanding = [client.result("scene.query", {"page_size": 1, "fields": []})
                           for _ in range(32)]
            self.assertEqual(client.result("project.status")["cursors"], {"retained": 32, "maximum": 32})
            self.error(client.call("scene.query", {"page_size": 1}), -32000, "conflict")
            self.assertEqual(client.result("project.status")["cursors"]["retained"], 32)
            page = outstanding[0]
            for _ in range(4):
                if page["complete"]:
                    break
                page = client.result("scene.continue", {"cursor": page["next_cursor"]})
            self.assertTrue(page["complete"])
            self.assertEqual(client.result("project.status")["cursors"]["retained"], 31)
            self.assertIsInstance(client.result("scene.query", {"page_size": 1})["next_cursor"], str)
            self.assertEqual(client.result("project.status")["cursors"]["retained"], 32)
            self.assertEqual(client.result("project.close"), {"open": False})
            client.finish()

    def test_failed_open_preserves_owner_and_close_invalidates_cursors(self):
        with self.client() as client:
            self.assertEqual(client.result("project.status"), {"open": False})
            opened = client.result("project.open", {"path": "FeatureWorld.sharkproject"})
            cursor = client.result("scene.query", {"page_size": 1})["next_cursor"]
            for path in ("../escape.sharkproject", "missing.sharkproject"):
                self.assertIn("error", client.call("project.open", {"path": path}))
                self.assertEqual(client.result("project.status")["revision"], opened["revision"])
            self.assertEqual(client.result("project.close"), {"open": False})
            reopened = client.result("project.open", {"path": "FeatureWorld.sharkproject"})
            self.assertNotEqual(reopened["revision"]["scene_instance"], opened["revision"]["scene_instance"])
            self.error(client.call("scene.continue", {"cursor": cursor}), -32000, "conflict")
            self.error(client.call("scene.continue", {"cursor": "30000000-0000-4000-8000-000000000001"}),
                       -32000, "conflict")
            self.assertEqual(client.result("project.status")["project_id"], PROJECT)
            client.finish()

    def test_second_writer_exclusion_eof_and_process_death_release_lock(self):
        with self.client(opened=True) as client:
            self.assertTrue(client.result("project.status")["open"])
            competing = self.finite("--request", encoded(request("project.open", {
                "path": "FeatureWorld.sharkproject"})), expected=1)
            self.error(competing, -32000, "conflict")
            client.finish()
        with self.client(opened=True) as client:
            self.assertTrue(client.result("project.status")["open"])
            client.process.kill()
            self.assertNotEqual(client.process.wait(timeout=1), 0)
        reopened = self.finite("--request", encoded(request("project.open", {
            "path": "FeatureWorld.sharkproject"})))
        self.assertEqual(reopened["result"]["project_id"], PROJECT)
        command = [str(ARGS.author), "--request", "--root", str(self.project),
                   "--project", "FeatureWorld.sharkproject"]
        with tempfile.TemporaryFile() as stderr:
            with subprocess.Popen(command, cwd=self.root, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=stderr) as process:
                try:
                    # Close the only stdout reader before asking the owned
                    # project for a response. EOF on stdin makes dispatch run.
                    process.stdout.close()
                    process.stdin.write(encoded(request("scene.query", {"fields": []})))
                    process.stdin.close()
                    self.assertEqual(process.wait(timeout=TIMEOUT), 2)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=1)
            stderr.seek(0)
            self.assertEqual(stderr.read(RESPONSE_BYTES + 1), b"")
        reopened = self.finite("--request", encoded(request("project.status")), opened=True)
        self.assertEqual(reopened["result"]["project_id"], PROJECT)

    def test_bad_requests_do_not_dispatch_batches_or_corrupt_later_streams(self):
        with self.client() as client:
            bad = [
                (b'{"jsonrpc":"2.0","jsonrpc":"2.0"}', -32700),
                (b"\xff", -32700),
                (b"\0", -32700),
                (b"{", -32700),
                (encoded([request("project.open", {"path": "FeatureWorld.sharkproject"}),
                          request("project.status")]), -32600),
                (encoded({**request("project.status"), "protocol_version": 2}), -32001),
                (encoded({**request("project.status"), "id": True}), -32600),
                (encoded(request("future.capture")), -32601),
                (encoded(request("project.status", {"unexpected": True})), -32602),
            ]
            for body, expected in bad:
                with self.subTest(body=body[:100]):
                    client.send(body + b"\n")
                    row = client.row()
                    self.error(row, expected)
                    if expected == -32001:
                        self.assertEqual(row["error"]["data"]["supported_versions"], [1])
                    self.assertEqual(client.result("project.status"), {"open": False})
            client.finish()

    def test_request_boundary_is_body_bytes_and_oversize_does_not_drain_stdin(self):
        with self.client() as client:
            body = encoded(request("project.status"))
            writer, failures = client.send_large(body + b" " * (REQUEST_BYTES - len(body)) + b"\r\n")
            self.assertEqual(client.row()["result"], {"open": False})
            writer.join(timeout=1)
            self.assertFalse(writer.is_alive() or failures, failures)
            client.finish()
        with self.client() as client:
            writer, _ = client.send_large(b"x" * (REQUEST_BYTES + 1))
            # No LF or EOF has been sent: overlong frames must fail immediately.
            self.assertIn("error", client.row())
            self.assertEqual(client.process.wait(timeout=TIMEOUT), 2)
            writer.join(timeout=1)
            self.assertFalse(writer.is_alive())
            client.finish(expected=2)
        truncated = self.finite("--stdio", encoded(request("project.status")), expected=2)
        self.assertIn("error", truncated)
        oversized = self.finite("--request", b"x" * (REQUEST_BYTES + 1), expected=2)
        self.assertIn("error", oversized)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", type=Path, required=True)
    parser.add_argument("--project", type=Path, default=ROOT / "assets/FeatureWorld/FeatureWorld.sharkproject")
    ARGS, remaining = parser.parse_known_args()
    ARGS.author, ARGS.project = ARGS.author.resolve(), ARGS.project.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
