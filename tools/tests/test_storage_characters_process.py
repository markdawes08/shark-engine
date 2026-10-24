"""Real mTLS character ownership, checkpoints and full server/store restart."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import redirect_stdout
import copy
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process


WORLD = "00000000-0000-4000-8000-000000000001"
ACCOUNTS = ("00000000-0000-4000-8000-000000000101", "00000000-0000-4000-8000-000000000102")
# Every phase starts a fresh server process, reopening the same real database.
PHASES = ("create-save", "restart-read", "replacement", "logout-expiry",
          "revoke-certificate", "revoke-account", "restart-denied")
IDENTITIES = {name: ("a", "b") if name in ("create-save", "restart-read", "restart-denied")
              else ("b",) if name == "revoke-account" else ("a",) for name in PHASES}
SERVER_COUNTS = {
    # accepted, rejected, created, loaded, saved, roster conflicts, revoked admission, stale account admission
    "create-save": (12, 4, 2, 4, 2, 2, 0, 0),
    "restart-read": (8, 3, 0, 2, 0, 0, 0, 0),
    "replacement": (8, 2, 0, 2, 1, 0, 0, 2),
    "logout-expiry": (7, 2, 0, 2, 0, 0, 0, 0),
    "revoke-certificate": (3, 2, 0, 1, 0, 0, 1, 0),
    "revoke-account": (3, 2, 0, 1, 0, 0, 1, 0),
    "restart-denied": (0, 4, 0, 0, 0, 0, 2, 0),
}
COUNT_KEYS = ("accepted", "rejected", "created", "loaded", "saved", "roster_conflicts",
              "admission_rejections", "stale_account_rejections")
CLIENT_COUNTS = {
    ("create-save", "a"): (6, 2), ("create-save", "b"): (6, 2),
    ("restart-read", "a"): (4, 1), ("restart-read", "b"): (4, 2),
    ("replacement", "a"): (8, 2), ("logout-expiry", "a"): (7, 2),
    ("revoke-certificate", "a"): (3, 2), ("revoke-account", "b"): (3, 2),
    ("restart-denied", "a"): (0, 2), ("restart-denied", "b"): (0, 2),
}
NEGATIVES = ("missing-phase", "duplicate-process", "wrong-mode", "nonzero-exit", "timeout",
             "cleanup-failure", "changed-store", "changed-character", "wrong-revision",
             "missing-revocation", "missing-fence", "empty-exception")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def uuid(value) -> bool:
    return isinstance(value, str) and bool(re.fullmatch(
        r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", value))


def integer(value, expected: int) -> bool:
    return type(value) is int and value == expected


def validate_scenarios(phases: list[dict]) -> None:
    require(isinstance(phases, list) and [row.get("name") for row in phases] == list(PHASES),
            "character phase inventory differs")
    original_store = None
    originals = None
    previous_hash = None
    database = None
    probe = None
    for phase in phases:
        name = phase["name"]
        require(phase.get("status") == "PASS", "character phase failed: " + name)
        processes = phase.get("processes")
        require(isinstance(processes, list) and len(processes) == len(IDENTITIES[name]) + 1,
                "character process count differs: " + name)
        expected_names = [name + ".client-" + identity for identity in IDENTITIES[name]] + [name + ".server"]
        require([row.get("name") for row in processes] == expected_names, "character process inventory differs: " + name)
        for row in processes:
            require(row.get("status") == "PASS" and integer(row.get("exit_code"), 0)
                    and row.get("timed_out") is False and row.get("cleanup_errors") == [],
                    "character child failed: " + row["name"])
            command = row.get("command")
            require(isinstance(command, list) and len(command) % 2 == 1 and len(command) >= 3
                    and all(isinstance(value, str) and value for value in command), "invalid character process command")
            arguments = dict(zip(command[1::2], command[2::2]))
            require(len(arguments) == (len(command) - 1) // 2, "duplicate character process option")
            role = "server" if row["name"].endswith(".server") else "client"
            require(arguments.get("--mode") == role and arguments.get("--scenario") == name,
                    "character process mode/scenario differs")
            require(arguments.get("--database") and arguments.get("--fixtures")
                    and arguments.get("--discovery") and arguments.get("--old-envelope"),
                    "character process inputs missing")
            if probe is None:
                probe, database = command[0], arguments["--database"]
            require(command[0] == probe and arguments["--database"] == database,
                    "character process changed probe or database")
            value = json.loads(row.get("stdout", "").splitlines()[-1])
            require(value == row.get("result") and value.get("status") == "PASS"
                    and value.get("role") == role and value.get("scenario") == name,
                    "character process JSON differs")
            if role == "client":
                identity = arguments.get("--identity")
                require(identity in IDENTITIES[name] and row["name"] == name + ".client-" + identity
                        and value.get("identity") == identity and value.get("authenticated") is True,
                        "character client identity differs")
                accepted, rejected = CLIENT_COUNTS[(name, identity)]
                require(integer(value.get("accepted"), accepted) and integer(value.get("rejected"), rejected),
                        "character client outcome counts differ")
            else:
                require(integer(value.get("authenticated"), 2), "character server did not authenticate two connections")
                for key, expected in zip(COUNT_KEYS, SERVER_COUNTS[name]):
                    require(integer(value.get(key), expected), name + ": incorrect server " + key)
        result = processes[-1]["result"]
        require(uuid(result.get("store_id")) and result.get("world_id") == WORLD
                and integer(result.get("schema_version"), 3), "character store metadata differs")
        if original_store is None:
            original_store = result["store_id"]
        require(result["store_id"] == original_store, "server restart changed StoreId")
        records = result.get("records")
        require(isinstance(records, list) and len(records) == 2
                and [row.get("account_id") for row in records] == list(ACCOUNTS), "character ownership inventory differs")
        for index, record in enumerate(records):
            require(uuid(record.get("character_id")) and uuid(record.get("entity_id"))
                    and record.get("world_id") == WORLD, "character identity invalid")
            version = 2 if index == 1 or name in ("create-save", "restart-read") else 3
            require(integer(record.get("revision"), version)
                    and record.get("name") == ("Saved character" if version == 2 else "Replacement character")
                    and isinstance(record.get("cell"), list) and all(type(value) is int for value in record["cell"])
                    and record.get("cell") == [version * 123456, -17, 4]
                    and record.get("offset") == [version + 0.125, 42.5, 1023.75],
                    "durable checkpoint state/revision differs")
        require(records[0]["character_id"] != records[1]["character_id"]
                and records[0]["entity_id"] != records[1]["entity_id"], "characters share durable identity")
        identities = [{key: row[key] for key in ("account_id", "character_id", "entity_id", "world_id")}
                      for row in records]
        if originals is None:
            originals = identities
        require(identities == originals, "restart or rejected operation changed character identity")
        accounts = sorted(result.get("accounts", []), key=lambda row: row["account_id"])
        certificates = sorted(result.get("certificates", []), key=lambda row: row["account_id"])
        account_revoked = name in ("revoke-account", "restart-denied")
        certificate_revoked = name in ("revoke-certificate", "revoke-account", "restart-denied")
        require(all(type(row.get("revoked")) is bool and integer(row.get("roster_revision"), 2)
                    for row in accounts) and all(type(row.get("revoked")) is bool for row in certificates),
                "durable registry fields have incorrect types")
        require(accounts == [{"account_id": ACCOUNTS[0], "revoked": False, "roster_revision": 2},
                             {"account_id": ACCOUNTS[1], "revoked": account_revoked, "roster_revision": 2}],
                "durable account revocation/roster differs")
        require(certificates == [{"account_id": ACCOUNTS[0], "revoked": certificate_revoked},
                                 {"account_id": ACCOUNTS[1], "revoked": False}],
                "durable certificate revocation differs")
        digest = phase.get("database_sha256")
        require(isinstance(digest, str) and bool(re.fullmatch(r"[0-9a-f]{64}", digest)), "database hash missing")
        if name in ("restart-read", "logout-expiry", "restart-denied"):
            require(digest == previous_hash, "read/rejection phase changed database bytes: " + name)
        previous_hash = digest


def validate_receipt(receipt: dict) -> None:
    require(receipt.get("status") == "PASS" and receipt.get("error") is None,
            "character process receipt failed")
    validate_scenarios(receipt.get("phases"))
    controls = receipt.get("supervisor_negative_controls")
    require(isinstance(controls, list) and [row.get("name") for row in controls] == list(NEGATIVES),
            "character supervisor controls incomplete")
    for row in controls:
        require(row.get("status") == "PASS" and integer(row.get("exit_code"), 1)
                and row.get("reported_status") == "FAIL" and row.get("summary_status") == "FAIL"
                and isinstance(row.get("error"), str) and bool(row["error"]), "character supervisor falsely passed")
        if row["name"] == "empty-exception":
            require(row["error"] == "AssertionError", "empty exception lost its failure type")


def execute(label: str, command: list[str]) -> dict:
    result = run_process(command, timeout=22)
    value = None
    try:
        value = json.loads(result.stdout.splitlines()[-1])
    except (IndexError, json.JSONDecodeError):
        pass
    good = result.returncode == 0 and not result.timed_out and not result.cleanup_errors
    good = good and isinstance(value, dict) and value.get("status") == "PASS"
    return {"name": label, "command": command, "exit_code": result.returncode,
            "timed_out": result.timed_out, "cleanup_errors": list(result.cleanup_errors),
            "stdout": result.stdout, "stderr": result.stderr, "result": value,
            "status": "PASS" if good else "FAIL"}


def run_scenarios(args) -> list[dict]:
    phases = []
    args.partial_phases = phases
    with tempfile.TemporaryDirectory(prefix="storage-characters-", dir=args.output.parent) as temporary:
        work = Path(temporary).resolve()
        database = work / "Durable characters \u4e16\u754c.db"
        old = work / "old-session-request.bin"
        with ThreadPoolExecutor(max_workers=3) as pool:
            for name in PHASES:
                ready = work / (name + "-ready.txt")
                common = ["--scenario", name, "--fixtures", str(args.fixtures.resolve()),
                          "--database", str(database), "--discovery", str(work / name),
                          "--old-envelope", str(old)]
                server = pool.submit(execute, name + ".server",
                                     [str(args.probe.resolve()), "--mode", "server", *common, "--ready", str(ready)])
                workers = []
                try:
                    deadline = time.monotonic() + 5
                    cadence = threading.Event()
                    port = None
                    while time.monotonic() < deadline:
                        try:
                            candidate = int(ready.read_text(encoding="utf-8").strip())
                            if 1 <= candidate <= 65535:
                                port = candidate
                                break
                        except (OSError, ValueError):
                            pass
                        require(not server.done(), "Character server exited before readiness: " + str(server.result()) if server.done() else "")
                        cadence.wait(0.01)
                    require(port is not None, "Character server readiness timed out")
                    workers = [pool.submit(execute, name + ".client-" + identity,
                                           [str(args.probe.resolve()), "--mode", "client", *common,
                                            "--port", str(port), "--identity", identity])
                               for identity in IDENTITIES[name]]
                finally:
                    clients = [worker.result(timeout=25) for worker in workers]
                    server_row = server.result(timeout=25)
                    processes = [*clients, server_row]
                    phase = {"name": name, "status": "FAIL", "processes": processes,
                             "database_sha256": hashlib.sha256(database.read_bytes()).hexdigest() if database.is_file() else None}
                    phases.append(phase)
                require(len(clients) == len(IDENTITIES[name]) and all(row["status"] == "PASS" for row in processes),
                        "Character phase failed: " + json.dumps(phase))
                phase["status"] = "PASS"
    return phases


def negative_controls(args, phases: list[dict]) -> list[dict]:
    controls = []
    for name in NEGATIVES:
        changed = copy.deepcopy(phases)
        if name == "missing-phase": changed.pop()
        elif name == "duplicate-process": changed[0]["processes"][1] = changed[0]["processes"][0]
        elif name == "wrong-mode": changed[0]["processes"][0]["command"][2] = "server"
        elif name == "nonzero-exit": changed[0]["processes"][0]["exit_code"] = 9
        elif name == "timeout": changed[0]["processes"][0]["timed_out"] = True
        elif name == "cleanup-failure": changed[0]["processes"][0]["cleanup_errors"] = ["controlled cleanup failure"]
        elif name in ("changed-store", "changed-character", "wrong-revision", "missing-revocation", "missing-fence"):
            target = changed[2 if name == "missing-fence" else -1]["processes"][-1]
            value = target["result"]
            if name == "changed-store": value["store_id"] = "12345678-1234-4123-8123-123456789abc"
            elif name == "changed-character": value["records"][0]["character_id"] = "12345678-1234-4123-8123-123456789abc"
            elif name == "wrong-revision": value["records"][0]["revision"] = 2
            elif name == "missing-revocation":
                next(row for row in value["accounts"] if row["account_id"] == ACCOUNTS[1])["revoked"] = False
            elif name == "missing-fence": value["stale_account_rejections"] = 0
            target["stdout"] = json.dumps(value) + "\n"

        def injected(_args):
            if name == "empty-exception":
                raise AssertionError()
            return changed

        output = args.output.with_name(args.output.stem + ".negative-" + name + ".json")
        capture = io.StringIO()
        with redirect_stdout(capture):
            code = main(["--probe", str(args.probe), "--fixtures", str(args.fixtures), "--output", str(output)],
                        scenario_runner=injected, run_controls=False)
        result = json.loads(output.read_text(encoding="utf-8"))
        summary = json.loads(capture.getvalue().splitlines()[-1])
        require(code == 1 and result["status"] == "FAIL" and summary["status"] == "FAIL"
                and result["error"] and result["error"] == summary["error"], "Supervisor accepted " + name)
        controls.append({"name": name, "status": "PASS", "exit_code": code,
                         "reported_status": result["status"], "summary_status": summary["status"], "error": result["error"]})
    return controls


def main(argv=None, *, scenario_runner=run_scenarios, run_controls=True) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    phases, controls, error = [], [], None
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        phases = scenario_runner(args)
        validate_scenarios(phases)
        if run_controls:
            controls = negative_controls(args, phases)
    except Exception as failure:
        error = str(failure) or type(failure).__name__
        phases = getattr(args, "partial_phases", phases)
    receipt = {"status": "FAIL" if error is not None else "PASS", "error": error,
               "phases": phases, "supervisor_negative_controls": controls,
               "scope": "actual local mTLS admission, server-owned checkpoint state and real server/store restart; no production roster wire protocol, economic retry or physical power-loss claim",
               "discovery": "Test-only local files carry generated IDs to fixture clients; actual roster/create authorization uses live server-side mTLS. No private keys or raw session envelopes enter this receipt."}
    if error is None and run_controls:
        try:
            validate_receipt(receipt)
        except Exception as failure:
            error = str(failure) or type(failure).__name__
            receipt.update(status="FAIL", error=error)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": receipt["status"], "phases": len(phases),
                      "processes": sum(len(row["processes"]) for row in phases), "error": error}))
    return 1 if error is not None else 0


if __name__ == "__main__":
    raise SystemExit(main())
