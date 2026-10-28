"""Connected migration, authenticated runtime recovery and isolated restored restart."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import redirect_stdout
import copy
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from control_report import temporary_control_report
from process_runner import run_process

from test_storage_migration_process import expected_snapshot, fixture_hashes, identity

MAX_CHILD_OUTPUT = 1024 * 1024
CHILD_TIMEOUT = 30
READY_TIMEOUT = 8
PHASES = ("prepare", "runtime", "commit-crash", "outage", "recover", "restore", "restored")
IDENTITIES = {"runtime": ("a", "b"), "commit-crash": ("a",),
              "outage": ("a", "b"), "recover": ("a", "b"), "restored": ("a", "b")}
COUNTER_KEYS = ("authenticated", "login_accepted", "login_rejected", "stale_session_rejected",
                "ready", "peak_retained", "vfs_failures", "recovery_fenced_sessions")
COUNTERS = {"runtime": (2, 2, 0, 0, 2, 2, 0, 0),
            "outage": (2, 2, 0, 2, 2, 1, None, 2),
            "recover": (2, 2, 0, 2, 2, 0, 0, 0),
            "restored": (2, 1, 1, 2, 1, 0, 0, 0)}
INPUT_KEYS = {"--database", "--pre-backup", "--snapshot", "--restored", "--session-a", "--session-b",
              "--network-fixtures", "--storage-fixtures"}
FILE_KEYS = {"database", "pre-backup", "snapshot", "restored", "session-a", "session-b"}
CHECKS = {
    "runtime": ("checkpoint_saved", "consumed_session_sequence_rejected",
                "concurrent_transfer_and_stale_outcome", "scene_unchanged"),
    "outage": ("checkpoint_reopened", "actual_vfs_write_failed", "faulted_admission_rejected",
               "explicit_recovery_fenced_sessions", "scene_unchanged"),
    "recover": ("original_outage_identity_reconciled", "retention_window_advanced",
                "durable_revocation_fenced_live_session", "scene_unchanged"),
    "restored": ("expired_operations_remain_expired", "changed_retained_payload_rejected",
                 "restored_revocation_rejected_login", "scene_unchanged"),
}
NEGATIVES = (
    "missing-phase", "duplicate-phase", "missing-child", "duplicate-child", "wrong-mode", "wrong-phase",
    "changed-database", "changed-readiness", "nonzero-exit", "boolean-exit", "timeout", "cleanup-failure",
    "stderr-output", "stdout-mismatch", "duplicate-json-key", "oversized-output", "changed-store",
    "changed-character", "lost-checkpoint", "duplicated-effect", "changed-receipt", "regressed-high-water",
    "lost-revocation", "missing-migration", "missing-concurrency", "missing-crash", "missing-outage",
    "missing-recovery-fence", "missing-replay", "lost-expiry", "reused-session", "changed-backup",
    "changed-original", "changed-restored", "boolean-counter", "boolean-state", "lost-terminal-rejection",
    "empty-exception",
)


def expected_outcomes(phase: str) -> list[dict]:
    values = {"runtime": [(1, 1, "committed", False), (2, 1, "stale_revision", False)],
              "outage": [(1, 2, "committed", True)],
              "recover": [(1, 3, "committed", False),
                          *((1, sequence, "stale_revision", False) for sequence in range(4, 11)),
                          (1, 11, "committed", False)],
              "restored": [(1, 11, "committed", True)]}
    return [{"actor": actor, "sequence": sequence, "status": status, "replayed": replayed}
            for actor, sequence, status, replayed in values[phase]]


def typed_receipt(actor: int, sequence: int, expected: int, quantity: tuple[int, int],
                  status: int, revision: int, source: tuple[int, int],
                  target: tuple[int, int]) -> list:
    other = 2 if actor == 1 else 1
    values = [expected, expected, *quantity, status, revision, *source, revision, *target]
    return [{"text": identity(actor)}, {"integer": sequence},
            {"text": identity(actor + 100)}, {"text": identity(other + 100)},
            *({"integer": value} for value in values)]


def expected_state(phase: str) -> dict:
    """Independent SQL-fixture oracle plus explicit, conserved operation equations."""
    require(phase in PHASES, "unknown integrated state")
    result = expected_snapshot(3, migrated_from=2)
    revision, balances = 1, ((100, 20), (100, 20))
    if phase != "prepare":
        row = result["characters"][0]
        row[3:] = [{"integer": 8}, {"text": "Integrated checkpoint"},
                   *({"integer": value} for value in (55, -8, 9)),
                   *({"real": value} for value in (0.5, 1.25, 777.75))]
        revision, balances = 2, ((93, 18), (107, 22))
        result["accounts"][0][3] = {"integer": 1}
        result["accounts"][1][3] = {"integer": 1}
        result["receipts"] = [typed_receipt(1, 1, 1, (7, 2), 0, 2, balances[0], balances[1]),
                              typed_receipt(2, 1, 1, (5, 1), 1, 2, balances[1], balances[0])]
    if phase in ("commit-crash", "outage"):
        revision, balances = 3, ((90, 17), (110, 23))
        result["accounts"][0][3] = {"integer": 2}
        result["receipts"].insert(1, typed_receipt(1, 2, 2, (3, 1), 0, 3,
                                                balances[0], balances[1]))
    elif phase in ("recover", "restore", "restored"):
        revision, balances = 5, ((84, 15), (116, 25))
        result["accounts"][0][3] = {"integer": 11}
        result["accounts"][1][1] = {"integer": 1}
        previous = ((86, 16), (114, 24))
        result["receipts"] = [typed_receipt(1, sequence, 1, (1, 0), 1, 4,
                                           previous[0], previous[1]) for sequence in range(4, 11)] + [
            typed_receipt(1, 11, 4, (2, 1), 0, 5, balances[0], balances[1]),
            typed_receipt(2, 1, 1, (5, 1), 1, 2, (107, 22), (93, 18))]
    result["economies"] = [[{"text": identity(owner + 100)}, {"integer": revision},
                            {"integer": currency}, {"integer": items}]
                           for owner, (currency, items) in enumerate(balances, 1)]
    require(sum(value[0] for value in balances) == 200 and sum(value[1] for value in balances) == 40,
            "independent conservation oracle differs")
    return result


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def same(actual, expected, message: str) -> None:
    require(json.dumps(actual, sort_keys=True, allow_nan=False) ==
            json.dumps(expected, sort_keys=True, allow_nan=False), message)


def integer(actual, expected: int) -> bool:
    return type(actual) is int and actual == expected


def digest(value) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def strict_json(text: str):
    require(isinstance(text, str) and 0 < len(text.encode("utf-8")) <= MAX_CHILD_OUTPUT,
            "integration child output exceeds its bound")

    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate integration JSON key")
            result[key] = value
        return result

    def invalid(value):
        raise ValueError("nonfinite integration JSON: " + value)

    return json.loads(text, object_pairs_hook=pairs, parse_constant=invalid)


def options(command: list[str]) -> dict:
    require(isinstance(command, list) and len(command) >= 3 and len(command) % 2 == 1
            and all(isinstance(value, str) and value for value in command),
            "integration child command shape differs")
    result = dict(zip(command[1::2], command[2::2]))
    require(len(result) == (len(command) - 1) // 2
            and all(key.startswith("--") for key in result),
            "duplicate or malformed integration child option")
    return result


def execute(name: str, command: list[str], expected_exit: int = 0) -> dict:
    try:
        result = run_process(command, timeout=CHILD_TIMEOUT)
    except Exception as error:
        return {"name": name, "command": command, "exit_code": -1, "timed_out": False,
                "cleanup_errors": [], "stdout": "", "stderr": str(error) or type(error).__name__,
                "result": None, "status": "FAIL"}
    value = None
    try:
        value = strict_json(result.stdout)
    except (AssertionError, ValueError, TypeError):
        pass
    good = (type(result.returncode) is int and result.returncode == expected_exit
            and not result.timed_out and not result.cleanup_errors and result.stderr == ""
            and len(result.stdout.encode("utf-8")) <= MAX_CHILD_OUTPUT
            and len(result.stderr.encode("utf-8")) <= MAX_CHILD_OUTPUT
            and isinstance(value, dict) and value.get("status") == "PASS")
    return {"name": name, "command": command, "exit_code": result.returncode,
            "timed_out": result.timed_out, "cleanup_errors": list(result.cleanup_errors),
            "stdout": result.stdout[:MAX_CHILD_OUTPUT], "stderr": result.stderr[:MAX_CHILD_OUTPUT],
            "result": value, "status": "PASS" if good else "FAIL"}


def ready_port(path: Path, server) -> int:
    deadline = time.monotonic() + READY_TIMEOUT
    cadence = threading.Event()
    while time.monotonic() < deadline:
        try:
            require(path.stat().st_size <= 16, "oversized integration readiness file")
            text = path.read_text(encoding="utf-8").strip()
            if re.fullmatch(r"[1-9][0-9]{0,4}", text) and 1 <= int(text) <= 65535:
                return int(text)
        except OSError:
            pass
        if server.done():
            raise AssertionError("integration server exited before readiness: " +
                                 json.dumps(server.result()))
        cadence.wait(min(0.01, max(0, deadline - time.monotonic())))
    raise AssertionError("integration server readiness deadline expired")


def file_digest(path: Path):
    if not path.is_file():
        return None
    require(path.stat().st_size <= 16 * 1024 * 1024, "integration database exceeds store bound")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_client(value: dict, phase: str, client: str, sessions: set) -> None:
    denied = phase == "restored" and client == "b"
    crash = phase == "commit-crash"
    require(value.get("client") == client
            and integer(value.get("accepted"), 0 if denied else 1 if crash else 2)
            and integer(value.get("rejected"), 2 if denied else 0 if phase == "runtime" else 1)
            and value.get("stale_rejected") is (phase != "runtime")
            and value.get("disconnect_before_ready") is crash
            and value.get("session_saved") is (phase == "runtime"),
            "integration client authentication/outcome differs")
    session = value.get("session")
    if denied:
        require(session is None, "revoked restored account acquired a session")
    else:
        require(isinstance(session, str) and re.fullmatch(
            r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", session)
                and session not in sessions, "session identity was missing or reused across restart")
        sessions.add(session)


def validate_scenarios(phases: list[dict]) -> None:
    require(isinstance(phases, list) and len(phases) == len(PHASES),
            "integrated phase inventory differs")
    inputs, probe, previous = None, None, None
    sessions = set()
    for phase, row in zip(PHASES, phases):
        require(row.get("name") == phase and row.get("status") == "PASS",
                "integrated phase order or status differs")
        children = row.get("processes")
        roles = ["client-" + client for client in IDENTITIES[phase]] + ["server"] if phase in IDENTITIES else [phase]
        require(isinstance(children, list) and len(children) == len(roles),
                "integrated child inventory differs")
        for child, role in zip(children, roles):
            mode = "client" if role.startswith("client-") else role
            exit_code = 73 if phase == "commit-crash" and role == "server" else 0
            require(child.get("name") == phase + "." + role and child.get("status") == "PASS"
                    and integer(child.get("exit_code"), exit_code) and child.get("timed_out") is False
                    and child.get("cleanup_errors") == [] and child.get("stderr") == "",
                    "integrated child did not finish with its exact expected outcome")
            arguments = options(child.get("command"))
            keys = INPUT_KEYS | {"--mode", "--phase", "--ready"} | ({"--client"} if mode == "client" else set())
            require(set(arguments) == keys and arguments["--mode"] == mode and arguments["--phase"] == phase,
                    "integrated child mode/phase/options differ")
            actual_inputs = {key: arguments[key] for key in INPUT_KEYS}
            if inputs is None:
                inputs, probe = actual_inputs, child["command"][0]
                parents = {str(PurePosixPath(inputs[key].replace("\\", "/")).parent)
                           for key in ("--database", "--pre-backup", "--snapshot", "--restored")}
                require(len(parents) == 4, "integration maintenance directories are not isolated")
                require(len({inputs["--" + key] for key in FILE_KEYS}) == len(FILE_KEYS),
                        "integration reused a database or session path")
            require(actual_inputs == inputs and child["command"][0] == probe,
                    "integration changed its store lineage, executable or fixture inputs")
            ready = PurePosixPath(arguments["--ready"].replace("\\", "/"))
            session_parent = PurePosixPath(inputs["--session-a"].replace("\\", "/")).parent
            require(ready.name == phase + "-ready.txt" and ready.parent == session_parent
                    and PurePosixPath(inputs["--session-b"].replace("\\", "/")).parent == session_parent,
                    "integration readiness belongs to another phase or lineage")
            value = strict_json(child.get("stdout"))
            same(value, child.get("result"), "integration stdout/result mismatch")
            require(isinstance(value, dict) and value.get("status") == "PASS"
                    and value.get("mode") == mode and value.get("phase") == phase,
                    "integrated child JSON identity differs")
            if mode == "client":
                client = role[-1]
                require(arguments["--client"] == client, "integrated client identity differs")
                validate_client(value, phase, client, sessions)
        value = children[-1]["result"]
        if phase == "prepare":
            same(value.get("legacy_snapshot"), expected_snapshot(2), "integration legacy fixture differs")
            same(value.get("pre_backup_snapshot"), expected_snapshot(2), "integration pre-upgrade backup differs")
            same(value.get("snapshot"), expected_state(phase), "integration migrated/initialized state differs")
        elif phase == "restore":
            same(value.get("pre_backup_snapshot"), expected_snapshot(2), "old backup was changed or upgraded")
            for key in ("source_snapshot", "snapshot", "candidate_snapshot"):
                same(value.get(key), expected_state(phase), "same-point isolated restoration differs: " + key)
        elif phase == "commit-crash":
            same(value, {"status": "PASS", "mode": "server", "phase": phase, "event": "after_commit",
                         "authenticated": 1, "ready": 1, "actor": 1, "sequence": 2,
                         "currency": 3, "items": 1, "expected_source_revision": 2,
                         "expected_target_revision": 2}, "committed lost acknowledgement was not observed")
        else:
            for key, count in zip(COUNTER_KEYS, COUNTERS[phase]):
                require((type(value.get(key)) is int and 1 <= value[key] <= 4096) if count is None
                        else integer(value.get(key), count), "integrated server count differs: " + key)
            same(value.get("snapshot"), expected_state(phase), "integrated complete durable state differs")
            same(value.get("checks"), list(CHECKS[phase]), "integrated behavioral check inventory differs")
            same(value.get("outcomes"), expected_outcomes(phase), "integrated original request outcomes differ")
        files = row.get("files")
        require(isinstance(files, dict) and set(files) == FILE_KEYS, "integration file hash inventory differs")
        for key, hashed in files.items():
            absent = (key in ("snapshot", "restored") and phase not in ("restore", "restored")) or (
                key in ("session-a", "session-b") and phase == "prepare")
            require(hashed is None if absent else digest(hashed), "integrated artifact hash missing/unexpected: " + key)
        if previous is not None:
            require(files["pre-backup"] == previous["files"]["pre-backup"], "original pre-upgrade backup changed")
            if phase != "runtime":
                require(all(files[key] == previous["files"][key] for key in ("session-a", "session-b")),
                        "original session bytes were replaced")
            if phase in ("restore", "restored"):
                require(files["database"] == previous["files"]["database"], "isolated restoration changed original database")
            if phase == "restored":
                require(all(files[key] == previous["files"][key] for key in ("snapshot", "restored")),
                        "read-only/replay restored restart changed durable bytes")
        previous = row


def run_scenarios(args) -> list[dict]:
    phases = []
    args.partial_phases = phases
    with tempfile.TemporaryDirectory(prefix="storage-integration-", dir=args.output.parent) as temporary:
        work = Path(temporary).resolve()
        paths = {"database": work / "live" / "Integrated world \u4e16\u754c.db",
                 "pre-backup": work / "pre-upgrade" / "Original backup.db",
                 "snapshot": work / "latest" / "Same-point snapshot.db",
                 "restored": work / "restored" / "Isolated candidate.db",
                 "session-a": work / "session-a.bin", "session-b": work / "session-b.bin"}
        for path in paths.values():
            path.parent.mkdir(parents=True, exist_ok=True)
        with ThreadPoolExecutor(max_workers=3) as pool:
            for phase in PHASES:
                ready = work / (phase + "-ready.txt")
                common = ["--phase", phase, *[item for key, path in paths.items()
                                               for item in ("--" + key, str(path))],
                          "--ready", str(ready), "--network-fixtures", str(args.fixtures.resolve()),
                          "--storage-fixtures", str(args.storage_fixtures.resolve())]
                command = [str(args.probe.resolve()), "--mode"]
                readiness_error = None
                if phase not in IDENTITIES:
                    children = [execute(phase + "." + phase, [*command, phase, *common])]
                else:
                    server = pool.submit(execute, phase + ".server", [*command, "server", *common],
                                         73 if phase == "commit-crash" else 0)
                    workers = []
                    try:
                        ready_port(ready, server)
                        workers = [pool.submit(execute, phase + ".client-" + identity,
                                               [*command, "client", *common, "--client", identity])
                                   for identity in IDENTITIES[phase]]
                    except Exception as error:
                        readiness_error = str(error) or type(error).__name__
                    finally:
                        children = [worker.result(timeout=CHILD_TIMEOUT + 5) for worker in workers]
                        children.append(server.result(timeout=CHILD_TIMEOUT + 5))
                row = {"name": phase, "status": "FAIL", "processes": children,
                       "files": {key: file_digest(path) for key, path in paths.items()}}
                phases.append(row)
                require(readiness_error is None, readiness_error or "integration readiness failed")
                expected = len(IDENTITIES[phase]) + 1 if phase in IDENTITIES else 1
                require(len(children) == expected and all(child["status"] == "PASS" for child in children),
                        "integrated phase failed: " + json.dumps(row))
                row["status"] = "PASS"
    return phases


def validate_receipt(receipt: dict) -> None:
    require(receipt.get("status") == "PASS" and receipt.get("error") is None,
            "integrated storage receipt failed")
    same(receipt.get("fixture_sources"), fixture_hashes(), "independent storage fixture binding differs")
    validate_scenarios(receipt.get("phases"))
    controls = receipt.get("supervisor_negative_controls")
    require(isinstance(controls, list) and [row.get("name") for row in controls] == list(NEGATIVES),
            "integrated supervisor controls incomplete")
    for row in controls:
        require(row.get("status") == "PASS" and integer(row.get("exit_code"), 1)
                and integer(row.get("runner_calls"), 1) and row.get("reported_status") == "FAIL"
                and row.get("summary_status") == "FAIL" and isinstance(row.get("error"), str) and row["error"],
                "integrated supervisor falsely passed a malformed result")
        if row["name"] == "empty-exception":
            require(row["error"] == "AssertionError", "empty exception lost its failure type")


def summary(receipt: dict) -> dict:
    """Compact, strictly validated dimensions for higher-level qualification receipts."""
    validate_receipt(receipt)
    phases = receipt["phases"]
    servers = [row["processes"][-1]["result"] for row in phases if row["name"] in IDENTITIES]
    return {"status": "PASS", "phases": len(phases),
            "processes": sum(len(row["processes"]) for row in phases),
            "authenticated_connections": sum(row["authenticated"] for row in servers),
            "supervisor_negative_controls": len(receipt["supervisor_negative_controls"]),
            "commit_crashes": 1, "vfs_outage_phases": 1,
            "fixture_sources": receipt["fixture_sources"]}


def negative_controls(args, phases: list[dict]) -> list[dict]:
    controls = []
    for name in NEGATIVES:
        changed = copy.deepcopy(phases)
        child = changed[1]["processes"][0]
        target = changed[-1]["processes"][-1]
        value = target["result"]
        synchronize = False
        if name == "missing-phase": changed.pop()
        elif name == "duplicate-phase": changed[1] = copy.deepcopy(changed[0])
        elif name == "missing-child": changed[1]["processes"].pop(0)
        elif name == "duplicate-child": changed[1]["processes"][1] = copy.deepcopy(child)
        elif name in ("wrong-mode", "wrong-phase", "changed-database", "changed-readiness"):
            key = {"wrong-mode": "--mode", "wrong-phase": "--phase", "changed-database": "--database",
                   "changed-readiness": "--ready"}[name]
            position = child["command"].index(key) + 1
            child["command"][position] = "prepare" if name in ("wrong-mode", "wrong-phase") else "changed/" + child["command"][position]
        elif name == "nonzero-exit": child["exit_code"] = 7
        elif name == "boolean-exit": child["exit_code"] = False
        elif name == "timeout": child["timed_out"] = True
        elif name == "cleanup-failure": child["cleanup_errors"] = ["controlled cleanup failure"]
        elif name == "stderr-output": child["stderr"] = "unexpected diagnostic"
        elif name == "stdout-mismatch": child["result"]["accepted"] = 99
        elif name == "duplicate-json-key": child["stdout"] = '{"status":"PASS",' + child["stdout"].lstrip()[1:]
        elif name == "oversized-output": child["stdout"] = " " * (MAX_CHILD_OUTPUT + 1) + child["stdout"]
        elif name == "changed-backup": changed[4]["files"]["pre-backup"] = "0" * 64
        elif name == "changed-original": changed[5]["files"]["database"] = "0" * 64
        elif name == "changed-restored": changed[6]["files"]["restored"] = "0" * 64
        elif name != "empty-exception":
            synchronize = True
            if name == "changed-store": value["snapshot"]["store"][0][1] = {"text": identity(701)}
            elif name == "changed-character": value["snapshot"]["characters"][0][2] = {"text": identity(999)}
            elif name == "lost-checkpoint": value["snapshot"]["characters"][0][3] = {"integer": 7}
            elif name == "duplicated-effect": value["snapshot"]["economies"][0][2] = {"integer": 80}
            elif name == "changed-receipt": value["snapshot"]["receipts"][-1][6] = {"integer": 6}
            elif name == "regressed-high-water": value["snapshot"]["accounts"][0][3] = {"integer": 3}
            elif name == "lost-revocation": value["snapshot"]["accounts"][1][1] = {"integer": 0}
            elif name == "missing-migration":
                target = changed[0]["processes"][-1]
                target["result"]["snapshot"]["user_version"] = [[{"integer": 2}]]
            elif name in ("missing-concurrency", "lost-terminal-rejection"):
                target = changed[1]["processes"][-1]
                if name == "missing-concurrency": target["result"]["peak_retained"] = 1
                else: target["result"]["outcomes"][1]["status"] = "committed"
            elif name == "missing-crash":
                target = changed[2]["processes"][-1]
                target["result"]["event"] = "before_commit"
            elif name in ("missing-outage", "missing-recovery-fence", "missing-replay"):
                target = changed[3]["processes"][-1]
                if name == "missing-outage": target["result"]["vfs_failures"] = 0
                elif name == "missing-recovery-fence": target["result"]["recovery_fenced_sessions"] = 0
                else: target["result"]["outcomes"][0]["replayed"] = False
            elif name == "lost-expiry": value["checks"].pop(0)
            elif name == "reused-session":
                target = changed[3]["processes"][0]
                target["result"]["session"] = child["result"]["session"]
            elif name == "boolean-counter": value["ready"] = True
            elif name == "boolean-state": value["snapshot"]["economies"][0][1] = {"integer": True}
            else: raise AssertionError("unimplemented integration control: " + name)
        if synchronize:
            target["stdout"] = json.dumps(target["result"]) + "\n"
        calls = 0

        def injected(_args):
            nonlocal calls
            calls += 1
            if name == "empty-exception":
                raise AssertionError()
            return changed

        destination = args.output.with_name(args.output.stem + ".negative-" + name + ".json")
        with temporary_control_report(destination) as output:
            capture = io.StringIO()
            with redirect_stdout(capture):
                code = main(["--probe", str(args.probe), "--fixtures", str(args.fixtures),
                             "--storage-fixtures", str(args.storage_fixtures), "--output", str(output)],
                            scenario_runner=injected, run_controls=False)
            result = json.loads(output.read_text(encoding="utf-8"))
            reported = json.loads(capture.getvalue().splitlines()[-1])
            require(calls == 1 and code == 1 and result["status"] == reported["status"] == "FAIL"
                    and result["error"] and result["error"] == reported["error"],
                    "integration supervisor accepted " + name)
            controls.append({"name": name, "status": "PASS", "exit_code": code, "runner_calls": calls,
                             "reported_status": result["status"], "summary_status": reported["status"],
                             "error": result["error"]})
    return controls


def main(argv=None, *, scenario_runner=run_scenarios, run_controls=True) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--storage-fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    phases, controls, error = [], [], None
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        hashes = {name: hashlib.sha256((args.storage_fixtures / name).read_bytes()).hexdigest()
                  for name in fixture_hashes()}
        same(hashes, fixture_hashes(), "supplied independent storage fixtures differ")
        phases = scenario_runner(args)
        validate_scenarios(phases)
        if run_controls:
            controls = negative_controls(args, phases)
    except Exception as failure:
        error = str(failure) or type(failure).__name__
        phases = getattr(args, "partial_phases", phases)
    receipt = {"status": "FAIL" if error is not None else "PASS", "error": error,
               "phases": phases, "supervisor_negative_controls": controls, "fixture_sources": fixture_hashes(),
               "scope": "one connected schema-2 migration and real mTLS/runtime store lineage, conserved concurrent outcomes, committed lost reply, observed native-VFS outage, original-ID recovery and isolated same-point restored restart; no physical power/disk-loss or production capacity claim",
               "limits": "Fixed probe output is validated at 1 MiB per child after process capture; this is not an OS stdout/disk quota. Children have 30-second deadlines, readiness 8 seconds, maximum three concurrent owned process trees. The pre-upgrade backup remains inspection-only after later acknowledged writes; only the final same-point snapshot is activated after the original owner stops."}
    if error is None and run_controls:
        try:
            validate_receipt(receipt)
        except Exception as failure:
            error = str(failure) or type(failure).__name__
            receipt.update(status="FAIL", error=error)
    args.output.write_text(json.dumps(receipt, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"status": receipt["status"], "phases": len(phases),
                      "processes": sum(len(row["processes"]) for row in phases), "error": error}))
    return 1 if error is not None else 0


if __name__ == "__main__":
    raise SystemExit(main())
