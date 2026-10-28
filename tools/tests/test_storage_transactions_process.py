"""Authenticated simultaneous transfers, retained retries, expiry and process restart."""
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
from control_report import temporary_control_report
from process_runner import run_process

WORLD = "00000000-0000-4000-8000-000000000001"
ACCOUNTS = ("00000000-0000-4000-8000-000000000101", "00000000-0000-4000-8000-000000000102")
PHASES = ("race-expire", "restart-read")
IDENTITIES = {name: ("a", "b") for name in PHASES}
COUNT_KEYS = ("authenticated", "accepted", "rejected", "committed", "stale", "replayed",
              "mismatch", "expired", "economy_queries", "stream_queries", "simultaneous_pending")
COUNTS = {"race-expire": (2, 34, 4, 1, 17, 4, 2, 2, 4, 4, 2),
          "restart-read": (2, 10, 4, 0, 0, 2, 2, 2, 2, 2, 0)}
NEGATIVES = ("missing-phase", "duplicate-process", "wrong-mode", "nonzero-exit", "timeout",
             "cleanup-failure", "changed-store", "changed-character", "wrong-revision",
             "duplicated-effect", "missing-barrier", "changed-receipt", "missing-retention",
             "regressed-high-watermark", "changed-restart", "empty-exception")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def uuid(value) -> bool:
    return isinstance(value, str) and bool(re.fullmatch(
        r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", value))


def integer(value, expected: int) -> bool:
    return type(value) is int and value == expected


def expected_receipt(store, records, actor, sequence, status):
    source, target = records[actor], records[1 - actor]
    return {"store_id": store, "actor": ACCOUNTS[actor], "sequence": sequence,
            "source_id": source["character_id"], "target_id": target["character_id"],
            "expected_source_revision": 1, "expected_target_revision": 1,
            "currency": 7, "item_units": 2, "status": status, "source": source, "target": target}


def validate_scenarios(phases: list[dict]) -> None:
    require(isinstance(phases, list) and [row.get("name") for row in phases] == list(PHASES),
            "transaction phase inventory differs")
    previous = None
    database = probe = fixtures = None
    for phase in phases:
        name = phase["name"]
        require(phase.get("status") == "PASS", "transaction phase failed: " + name)
        processes = phase.get("processes")
        require(isinstance(processes, list) and len(processes) == 3, "transaction child count differs")
        require([row.get("name") for row in processes] ==
                [name + ".client-a", name + ".client-b", name + ".server"], "transaction process inventory differs")
        for row in processes:
            require(row.get("status") == "PASS" and integer(row.get("exit_code"), 0)
                    and row.get("timed_out") is False and row.get("cleanup_errors") == [],
                    "transaction child failed: " + row["name"])
            command = row.get("command")
            require(isinstance(command, list) and len(command) % 2 == 1 and len(command) >= 3
                    and all(isinstance(value, str) and value for value in command), "invalid transaction command")
            arguments = dict(zip(command[1::2], command[2::2]))
            require(len(arguments) == (len(command) - 1) // 2, "duplicate transaction process option")
            role = "server" if row["name"].endswith(".server") else "client"
            required = {"--mode", "--scenario", "--fixtures", "--database", "--discovery"}
            required |= {"--ready"} if role == "server" else {"--port", "--identity"}
            require(set(arguments) == required and arguments["--mode"] == role
                    and arguments["--scenario"] == name, "transaction mode/scenario/inputs differ")
            if probe is None:
                probe, database, fixtures = command[0], arguments["--database"], arguments["--fixtures"]
            require((command[0], arguments["--database"], arguments["--fixtures"]) == (probe, database, fixtures),
                    "transaction process changed executable, database or fixtures")
            value = json.loads(row.get("stdout", "").splitlines()[-1])
            require(value == row.get("result") and value.get("status") == "PASS"
                    and value.get("role") == role and value.get("scenario") == name, "transaction JSON differs")
            if role == "client":
                identity = arguments["--identity"]
                require(identity in ("a", "b") and row["name"] == name + ".client-" + identity
                        and value.get("identity") == identity and value.get("authenticated") is True,
                        "transaction client identity differs")
                require(integer(value.get("accepted"), 17 if name == "race-expire" else 5)
                        and integer(value.get("rejected"), 2), "transaction client outcome counts differ")
            else:
                for key, count in zip(COUNT_KEYS, COUNTS[name]):
                    require(integer(value.get(key), count), "transaction server count differs: " + key)
        result = processes[-1]["result"]
        require(uuid(result.get("store_id")) and result.get("world_id") == WORLD
                and integer(result.get("schema_version"), 3), "transaction metadata differs")
        records = result.get("records")
        require(isinstance(records, list) and len(records) == 2, "economy record count differs")
        for index, record in enumerate(records):
            require(set(record) == {"account_id", "character_id", "revision", "currency", "item_units"}
                    and record["account_id"] == ACCOUNTS[index] and uuid(record["character_id"])
                    and integer(record["revision"], 2)
                    and integer(record["currency"], (93, 107)[index])
                    and integer(record["item_units"], (18, 22)[index]), "economic conservation or ownership differs")
        require(records[0]["character_id"] != records[1]["character_id"], "characters share identity")
        expected = [{"actor": owner, "high_watermark": 9,
                     "receipts": [expected_receipt(result["store_id"], records, index, sequence, "stale_revision")
                                  for sequence in range(2, 10)]} for index, owner in enumerate(ACCOUNTS)]
        # Canonical JSON equality also rejects bool/float substitutions for integer receipt fields.
        require(json.dumps(result.get("streams"), sort_keys=True) == json.dumps(expected, sort_keys=True),
                "retained outcomes, high watermark or bounded request binding differs")
        first = [expected_receipt(result["store_id"], records, index, 1,
                                  "committed" if index == 0 else "stale_revision") for index in range(2)]
        require(json.dumps(result.get("initial_receipts"), sort_keys=True) ==
                json.dumps(first if name == "race-expire" else [], sort_keys=True), "initial race outcome differs")
        digest = phase.get("database_sha256")
        require(isinstance(digest, str) and bool(re.fullmatch(r"[0-9a-f]{64}", digest)), "database hash missing")
        if previous is not None:
            for key in ("store_id", "records", "streams"):
                require(result[key] == previous["processes"][-1]["result"][key], "restart changed " + key)
            require(digest == previous["database_sha256"], "read/replay/rejection changed database bytes")
        previous = phase


def validate_receipt(receipt: dict) -> None:
    require(receipt.get("status") == "PASS" and receipt.get("error") is None, "transaction receipt failed")
    validate_scenarios(receipt.get("phases"))
    controls = receipt.get("supervisor_negative_controls")
    require(isinstance(controls, list) and [row.get("name") for row in controls] == list(NEGATIVES),
            "transaction negative controls incomplete")
    for row in controls:
        require(row.get("status") == "PASS" and integer(row.get("exit_code"), 1)
                and row.get("reported_status") == "FAIL" and row.get("summary_status") == "FAIL"
                and integer(row.get("runner_calls"), 1) and isinstance(row.get("error"), str) and row["error"],
                "transaction supervisor falsely passed")
        if row["name"] == "empty-exception":
            require(row["error"] == "AssertionError", "empty exception lost failure type")


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
    with tempfile.TemporaryDirectory(prefix="storage-transactions-", dir=args.output.parent) as temporary:
        work = Path(temporary).resolve()
        database = work / "Durable transactions \u4e16\u754c.db"
        with ThreadPoolExecutor(max_workers=3) as pool:
            for name in PHASES:
                ready = work / (name + "-ready.txt")
                common = ["--scenario", name, "--fixtures", str(args.fixtures.resolve()),
                          "--database", str(database), "--discovery", str(work / name)]
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
                        require(not server.done(), "Transaction server exited before readiness: " + str(server.result()) if server.done() else "")
                        cadence.wait(0.01)
                    require(port is not None, "Transaction server readiness timed out")
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
                        "Transaction phase failed: " + json.dumps(phase))
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
        elif name == "changed-restart": changed[-1]["database_sha256"] = "f" * 64
        elif name != "empty-exception":
            target = changed[0 if name == "missing-barrier" else -1]["processes"][-1]
            value = target["result"]
            if name == "changed-store": value["store_id"] = "12345678-1234-4123-8123-123456789abc"
            elif name == "changed-character": value["records"][0]["character_id"] = "12345678-1234-4123-8123-123456789abc"
            elif name == "wrong-revision": value["records"][0]["revision"] = 3
            elif name == "duplicated-effect": value["records"][0]["currency"] = 86
            elif name == "missing-barrier": value["simultaneous_pending"] = 0
            elif name == "changed-receipt": value["streams"][0]["receipts"][-1]["currency"] = 8
            elif name == "missing-retention": value["streams"][0]["receipts"].pop()
            elif name == "regressed-high-watermark": value["streams"][0]["high_watermark"] = 8
            target["stdout"] = json.dumps(value) + "\n"
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
                code = main(["--probe", str(args.probe), "--fixtures", str(args.fixtures), "--output", str(output)],
                            scenario_runner=injected, run_controls=False)
            result = json.loads(output.read_text(encoding="utf-8"))
            summary = json.loads(capture.getvalue().splitlines()[-1])
            require(calls == 1 and code == 1 and result["status"] == "FAIL" and summary["status"] == "FAIL"
                    and result["error"] and result["error"] == summary["error"], "Supervisor accepted " + name)
            controls.append({"name": name, "status": "PASS", "exit_code": code, "runner_calls": calls,
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
               "scope": "two authenticated clients with simultaneous pending transfers, exact retained retry outcomes, expiry fences and full server/store restart; no production economy wire protocol or physical power-loss claim",
               "discovery": "Test-only local files carry generated character IDs. Fixed server-side stages use actual mTLS/session admission; private economic outcomes remain server-side. No private keys or session envelopes enter this receipt."}
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


