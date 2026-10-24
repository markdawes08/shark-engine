"""Qualify real store restarts, owner exclusion and interrupted initial commits."""
from __future__ import annotations

import argparse
from contextlib import redirect_stdout
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process


WORLD_ID = "00000000-0000-4000-8000-000000000001"
NEGATIVE_CONTROLS = ("invalid-store-id", "wrong-world", "future-schema", "reported-failure",
                     "nonzero-exit", "timeout", "cleanup-failure", "empty-exception")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def metadata(value: dict) -> dict:
    require(isinstance(value, dict), "metadata must be an object")
    identity = value.get("store_id")
    require(isinstance(identity, str) and bool(re.fullmatch(
        r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", identity)),
        "metadata store ID is not a canonical UUIDv4")
    require(value.get("world_id") == WORLD_ID, "metadata world differs")
    require(type(value.get("schema_version")) is int and value["schema_version"] == 2,
            "metadata schema version differs")
    return {key: value[key] for key in ("store_id", "world_id", "schema_version")}


def expected_rows() -> list[tuple[str, str | None, int, str | None]]:
    rows = [
        ("create", "create", 0, None), ("reopen", "read", 0, None),
        ("restart-stable-metadata", None, 0, None),
        ("create-existing", "check-create-existing", 0, None),
        ("existing-file-unchanged", None, 0, None), ("owner-ready", None, 0, None),
        ("second-owner-rejected", "check-busy", 0, None),
        ("normal-owner-release", None, 0, None), ("read-after-release", "read", 0, None),
        ("normal-release-reopen", None, 0, None),
        ("abrupt-owner-exit", "abort-owner", 73, None),
        ("read-after-abrupt-exit", "read", 0, None),
        ("abrupt-release-reopen", None, 0, None),
        ("hot-journal.interrupt", "hot-journal-exit", 73, None),
        ("hot-journal.supported-header", None, 0, None),
        ("hot-journal.nonzero-synced-header", None, 0, None),
        ("hot-journal.reopen", "read", 0, None),
        ("hot-journal.original-metadata-restored", None, 0, None),
        ("hot-journal.second-reopen", "read", 0, None),
        ("hot-journal.second-reopen-stable", None, 0, None),
        ("hot-journal.recovery-completed", None, 0, None),
    ]
    for phase in ("schema_written", "before_commit", "after_commit"):
        rows.extend([
            (phase + ".interrupt", "create-exit", 73, phase),
            (phase + ".allocated-file", None, 0, None),
            (phase + ".create-preserves-allocation", "check-create-existing", 0, None),
            (phase + ".allocation-unchanged", None, 0, None),
        ])
        if phase == "after_commit":
            rows.extend([(phase + ".reopen", "read", 0, None),
                         (phase + ".second-reopen", "read", 0, None),
                         (phase + ".durable-identity", None, 0, None)])
        else:
            rows.extend([(phase + ".uninitialized-rejected", "check-uninitialized", 0, None),
                         (phase + ".uninitialized-still-rejected", "check-uninitialized", 0, None),
                         (phase + ".uninitialized-files-retained", None, 0, None),
                         (phase + ".explicit-create", "create", 0, None),
                         (phase + ".recovered-read", "read", 0, None),
                         (phase + ".recovered-reopen", None, 0, None)])
    return rows


def validate_receipt(receipt: dict) -> None:
    """Reject incomplete or inconsistent copied qualification receipts."""
    require(receipt.get("status") == "PASS" and receipt.get("error") is None,
            "storage process receipt did not pass")
    rows = receipt.get("rows")
    expected = expected_rows()
    require(isinstance(rows, list) and len(rows) == len(expected), "storage process row count differs")
    identities: dict[str, dict] = {}
    commands: dict[str, list[str]] = {}
    for row, (name, mode, exit_code, phase) in zip(rows, expected):
        require(row.get("name") == name and row.get("status") == "PASS", "storage process row differs: " + name)
        if mode is None:
            continue
        command = row.get("command")
        require(isinstance(command, list) and all(isinstance(value, str) and value for value in command)
                and len(command) == (4 if phase else 3) and command[1] == mode
                and (phase is None or command[3] == phase), "storage process command differs: " + name)
        require(type(row.get("exit_code")) is int and row["exit_code"] == exit_code
                and row.get("timed_out") is False and row.get("cleanup_errors") == [],
                "storage process child result differs: " + name)
        commands[name] = command
        if exit_code == 0:
            value = json.loads(row["stdout"].splitlines()[-1])
            require(value.get("status") == "PASS", "storage process JSON did not pass: " + name)
            if mode in ("create", "read"):
                require(value.get("state") == "complete", "storage process state differs: " + name)
                identities[name] = metadata(value)
            else:
                require(value.get("code") == ("invalid_configuration" if mode == "check-uninitialized"
                                             else "conflict"), "storage process rejection differs: " + name)
    original = identities["create"]
    for name in ("reopen", "read-after-release", "read-after-abrupt-exit",
                 "hot-journal.reopen", "hot-journal.second-reopen"):
        require(identities[name] == original, "restart changed durable metadata: " + name)
    by_name = {row["name"]: row for row in rows}
    for name in ("restart-stable-metadata", "owner-ready", "hot-journal.original-metadata-restored"):
        require(metadata(by_name[name]["metadata"]) == original, "recorded metadata differs: " + name)
    released = json.loads(by_name["normal-owner-release"]["stdout"].splitlines()[-1])
    require(released.get("status") == "PASS" and released.get("state") == "complete"
            and metadata(released) == original, "normal owner release result differs")
    require(bool(re.fullmatch(r"[0-9a-f]{64}", by_name["existing-file-unchanged"].get("sha256", ""))),
            "missing existing-file hash")
    journal = by_name["hot-journal.nonzero-synced-header"]
    require(type(journal.get("bytes")) is int and journal["bytes"] > 28
            and bool(re.fullmatch(r"[0-9a-f]{64}", journal.get("sha256", ""))), "missing hot-journal evidence")
    for phase in ("schema_written", "before_commit", "after_commit"):
        files = by_name[phase + ".allocation-unchanged"].get("files")
        require(isinstance(files, dict) and phase + ".db" in files
                and set(files).issubset({phase + ".db", phase + ".db-journal"})
                and all(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
                        for value in files.values()), "missing allocated-file hashes: " + phase)
        first, second = ((phase + ".reopen", phase + ".second-reopen") if phase == "after_commit"
                         else (phase + ".explicit-create", phase + ".recovered-read"))
        require(identities[first] == identities[second] and identities[first]["store_id"] != original["store_id"],
                "interrupted creation recovery changed metadata: " + phase)
        if phase == "after_commit":
            require(metadata(by_name[phase + ".durable-identity"]["metadata"]) == identities[first],
                    "committed creation metadata differs")
    base_command = commands["create"]
    for name, command in commands.items():
        require(command[0] == base_command[0], "probe changed within receipt")
        phase = next((value for value in ("schema_written", "before_commit", "after_commit")
                      if name.startswith(value + ".")), None)
        if phase is None:
            require(command[2] == base_command[2], "database path changed within scenario")
        else:
            require(command[2] == commands[phase + ".interrupt"][2]
                    and command[2].replace("\\", "/").rsplit("/", 1)[-1] == phase + ".db",
                    "interruption database path differs")
    controls = receipt.get("supervisor_negative_controls")
    require(isinstance(controls, list) and [row.get("name") for row in controls] == list(NEGATIVE_CONTROLS),
            "missing supervisor negative controls")
    for control in controls:
        require(control.get("status") == "PASS" and type(control.get("exit_code")) is int
                and control["exit_code"] == 1 and control.get("reported_status") == "FAIL"
                and control.get("summary_status") == "FAIL" and isinstance(control.get("error"), str)
                and bool(control["error"]), "supervisor false-pass control failed")
        if control["name"] == "empty-exception":
            require(control["error"] == "AssertionError", "empty exception did not retain its failure type")


def negative_controls(probe: Path, output: Path) -> list[dict]:
    rows = []
    for name in NEGATIVE_CONTROLS:
        calls = 0

        def fake_runner(_command, *, timeout):
            nonlocal calls
            calls += 1
            require(timeout == 15, "unexpected negative-control timeout")
            if name == "empty-exception":
                raise AssertionError()
            value = {"status": "PASS", "state": "complete", "store_id": "12345678-1234-4123-8123-123456789abc",
                     "world_id": WORLD_ID, "schema_version": 2}
            if name == "invalid-store-id":
                value["store_id"] = "invalid"
            elif name == "wrong-world":
                value["world_id"] = "00000000-0000-4000-8000-000000000002"
            elif name == "future-schema":
                value["schema_version"] = 3
            elif name == "reported-failure":
                value["status"] = "FAIL"
            return SimpleNamespace(returncode=9 if name == "nonzero-exit" else 0,
                                   stdout=json.dumps(value) + "\n", stderr="",
                                   timed_out=name == "timeout",
                                   cleanup_errors=["controlled cleanup failure"] if name == "cleanup-failure" else [])

        report_path = output.with_name(output.stem + ".negative-" + name + ".json")
        captured = io.StringIO()
        with redirect_stdout(captured):
            result = main(["--probe", str(probe), "--output", str(report_path)],
                          runner=fake_runner, run_controls=False)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        summary = json.loads(captured.getvalue().splitlines()[-1])
        require(calls == 1 and result == 1 and report["status"] == "FAIL" and summary["status"] == "FAIL"
                and isinstance(report["error"], str) and bool(report["error"])
                and summary["error"] == report["error"], "supervisor falsely accepted " + name)
        expected_error = {"invalid-store-id": "metadata store ID is not a canonical UUIDv4",
                          "wrong-world": "metadata world differs", "future-schema": "metadata schema version differs",
                          "empty-exception": "AssertionError"}.get(name)
        require(expected_error is None or report["error"] == expected_error,
                "supervisor rejected the wrong condition for " + name)
        rows.append({"name": name, "status": "PASS", "exit_code": result, "reported_status": report["status"],
                     "summary_status": summary["status"], "error": report["error"]})
    return rows


def main(argv=None, *, runner=run_process, run_controls: bool = True) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    probe = args.probe.resolve()
    rows: list[dict] = []

    def check(name: str, condition: bool, **details) -> None:
        rows.append({"name": name, "status": "PASS" if condition else "FAIL", **details})
        if not condition:
            raise AssertionError(name)

    def run(name: str, mode: str, path: Path, *extra: str, expected: int = 0) -> dict | None:
        command = [str(probe), mode, str(path), *extra]
        result = runner(command, timeout=15)
        row = {"name": name, "command": command, "exit_code": result.returncode,
               "timed_out": result.timed_out, "cleanup_errors": list(result.cleanup_errors),
               "stdout": result.stdout, "stderr": result.stderr, "status": "FAIL"}
        rows.append(row)
        require(result.returncode == expected and not result.timed_out and not result.cleanup_errors, str(row))
        value = None
        if expected == 0:
            value = json.loads(result.stdout.splitlines()[-1])
            require(value["status"] == "PASS", str(row))
        row["status"] = "PASS"
        return value

    error = None
    controls = []
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if run_controls:
            controls = negative_controls(probe, args.output)
        with tempfile.TemporaryDirectory(prefix="storage-process-", dir=args.output.parent) as temporary:
            work = Path(temporary).resolve()
            # Pass Unicode and spaces directly as arguments, never through a shell.
            path = work / "Durable 世界 store.db"
            original = metadata(run("create", "create", path))
            reopened = metadata(run("reopen", "read", path))
            check("restart-stable-metadata", original == reopened, metadata=original)
            before = path.read_bytes()
            run("create-existing", "check-create-existing", path)
            check("existing-file-unchanged", path.read_bytes() == before,
                  sha256=hashlib.sha256(before).hexdigest())

            holder = subprocess.Popen([str(probe), "hold", str(path)], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                      encoding="utf-8")
            try:
                with ThreadPoolExecutor(max_workers=1) as pool:
                    ready = pool.submit(holder.stdout.readline)
                    try:
                        value = json.loads(ready.result(timeout=15))
                    except BaseException:
                        holder.kill()
                        holder.wait(timeout=5)
                        raise
                check("owner-ready", value["status"] == "PASS" and value["state"] == "ready"
                      and metadata(value) == original, metadata=metadata(value))
                run("second-owner-rejected", "check-busy", path)
                stdout, stderr = holder.communicate("release\n", timeout=15)
                released = json.loads(stdout.splitlines()[-1])
                check("normal-owner-release", holder.returncode == 0
                      and released["status"] == "PASS" and released["state"] == "complete"
                      and metadata(released) == original,
                      stdout=stdout, stderr=stderr)
            finally:
                if holder.poll() is None:
                    holder.kill()
                    holder.wait(timeout=5)
                for stream in (holder.stdin, holder.stdout, holder.stderr):
                    if stream is not None:
                        stream.close()
            check("normal-release-reopen", metadata(run("read-after-release", "read", path)) == original)
            run("abrupt-owner-exit", "abort-owner", path, expected=73)
            check("abrupt-release-reopen", metadata(run("read-after-abrupt-exit", "read", path)) == original)

            run("hot-journal.interrupt", "hot-journal-exit", path, expected=73)
            header = path.read_bytes()[:100]
            check("hot-journal.supported-header", len(header) == 100
                  and header[:16] == b"SQLite format 3\0"
                  and int.from_bytes(header[68:72], "big") == 0x53484B44
                  and int.from_bytes(header[60:64], "big") == 2
                  and header[16:20] == b"\x10\x00\x01\x01")
            hot_journal = Path(str(path) + "-journal")
            journal_bytes = hot_journal.read_bytes()
            check("hot-journal.nonzero-synced-header", len(journal_bytes) > 28
                  and journal_bytes[:8] == bytes.fromhex("d9d505f920a163d7"),
                  bytes=len(journal_bytes), sha256=hashlib.sha256(journal_bytes).hexdigest())
            check("hot-journal.original-metadata-restored",
                  metadata(run("hot-journal.reopen", "read", path)) == original,
                  metadata=original)
            check("hot-journal.second-reopen-stable",
                  metadata(run("hot-journal.second-reopen", "read", path)) == original)
            check("hot-journal.recovery-completed", not hot_journal.exists())

            for phase in ("schema_written", "before_commit", "after_commit"):
                interrupted = work / (phase + ".db")
                journal = Path(str(interrupted) + "-journal")
                run(phase + ".interrupt", "create-exit", interrupted, phase, expected=73)
                check(phase + ".allocated-file", interrupted.is_file())
                before_retry = {item.name: hashlib.sha256(item.read_bytes()).hexdigest()
                                for item in (interrupted, journal) if item.is_file()}
                run(phase + ".create-preserves-allocation", "check-create-existing", interrupted)
                after_retry = {item.name: hashlib.sha256(item.read_bytes()).hexdigest()
                               for item in (interrupted, journal) if item.is_file()}
                check(phase + ".allocation-unchanged", before_retry == after_retry,
                      files=after_retry)
                if phase == "after_commit":
                    committed = metadata(run(phase + ".reopen", "read", interrupted))
                    again = metadata(run(phase + ".second-reopen", "read", interrupted))
                    check(phase + ".durable-identity", committed == again
                          and committed["store_id"] != original["store_id"], metadata=committed)
                else:
                    run(phase + ".uninitialized-rejected", "check-uninitialized", interrupted)
                    run(phase + ".uninitialized-still-rejected", "check-uninitialized", interrupted)
                    check(phase + ".uninitialized-files-retained",
                          {item.name: hashlib.sha256(item.read_bytes()).hexdigest()
                           for item in (interrupted, journal) if item.is_file()} == after_retry)
                    # An explicit operator decision removes only these known fixture files.
                    for item in (interrupted, journal):
                        require(item.resolve().parent == work, "cleanup path escaped the fixture root")
                        if item.exists():
                            item.unlink()
                    recovered = metadata(run(phase + ".explicit-create", "create", interrupted))
                    check(phase + ".recovered-reopen",
                          metadata(run(phase + ".recovered-read", "read", interrupted)) == recovered)
    except Exception as failure:
        error = str(failure) or type(failure).__name__
    report = {"status": "FAIL" if error is not None else "PASS", "rows": rows, "error": error,
              "supervisor_negative_controls": controls,
              "scope": "local process exit and SQLite commit visibility; not hardware power loss or character/economy durability"}
    if error is None:
        try:
            validate_receipt(report)
        except Exception as failure:
            error = str(failure) or type(failure).__name__
            report.update(status="FAIL", error=error)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "checks": len(rows), "error": error}))
    return 1 if error is not None else 0


if __name__ == "__main__":
    raise SystemExit(main())
