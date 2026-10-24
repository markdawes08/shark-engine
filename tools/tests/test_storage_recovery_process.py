"""Real Store process interruption and native SQLite I/O outage recovery."""
from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import copy
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process

MUTATIONS = ("provision", "enroll", "revoke-account", "revoke-certificate",
             "create-character", "save-character", "initialize-economy", "transfer")
CRASHES = ("after_write", "before_commit", "after_commit", "after_reply")
OUTAGES = ("write-outage", "sync-outage", "full-outage")
RECIPES = tuple((mutation, phase) for mutation in MUTATIONS for phase in CRASHES)
RECIPES += (("transfer", "after_source_debit"),)
RECIPES += tuple((mutation, phase) for mutation in
                 ("transfer-reject", "transfer-prune", "transfer-prune-reject") for phase in CRASHES)
RECIPES += tuple((mutation, phase) for mutation in MUTATIONS for phase in OUTAGES)
NEGATIVES = ("missing-row", "duplicate-row", "wrong-mutation", "wrong-phase", "wrong-child-mode",
             "wrong-database", "nonzero-exit", "timeout", "cleanup-failure", "changed-store",
             "lost-acknowledged-effect", "partial-transfer", "duplicate-retry-effect",
             "missing-vfs-failure", "changed-retention", "wrong-replay", "reused-fresh-seed",
             "empty-exception")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def identity(number: int) -> str:
    return f"00000000-0000-4000-8000-{number:012x}"


WORLD = identity(900)
ACCOUNTS = (identity(1), identity(2))


def uuid(value) -> bool:
    return isinstance(value, str) and bool(re.fullmatch(
        r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", value))


def same(actual, expected, message: str) -> None:
    # Typed JSON comparison rejects booleans/floats substituted for integer fields.
    require(json.dumps(actual, sort_keys=True) == json.dumps(expected, sort_keys=True), message)


def source_character(snapshot, second=False):
    return next(row for row in snapshot["characters"]
                if row["account"] == ACCOUNTS[int(second)] and row["name"] != "Created")


def economy(snapshot, second=False):
    character = source_character(snapshot, second)["id"]
    return next(row for row in snapshot["economies"] if row["character"] == character)


def apply_transfer(snapshot, sequence: int, quantity: tuple[int, int], rejected=False):
    source, target = economy(snapshot), economy(snapshot, True)
    receipt = {"store": snapshot["store"], "actor": ACCOUNTS[0], "sequence": sequence,
               "source": source["character"], "target": target["character"],
               "expected_source_revision": source["revision"],
               "expected_target_revision": target["revision"],
               "currency": quantity[0], "items": quantity[1],
               "status": "insufficient_assets" if rejected else "committed"}
    if not rejected:
        for row, direction in ((source, -1), (target, 1)):
            row["revision"] += 1
            row["currency"] += direction * quantity[0]
            row["items"] += direction * quantity[1]
    receipt.update(source_record=copy.deepcopy(source), target_record=copy.deepcopy(target))
    stream = snapshot["streams"][0]
    stream["high_watermark"] = sequence
    stream["receipts"] = [*stream["receipts"], receipt][-8:]


def validate_seed(snapshot, mutation):
    require(isinstance(snapshot, dict) and uuid(snapshot.get("store")), "seed store identity invalid")
    characters = snapshot.get("characters")
    require(isinstance(characters, list) and len(characters) == 2, "seed character inventory differs")
    all_ids = [snapshot["store"]]
    expected_characters = []
    for index, owner in enumerate(ACCOUNTS):
        rows = [row for row in characters if row.get("account") == owner]
        require(len(rows) == 1 and uuid(rows[0].get("id")) and uuid(rows[0].get("entity")),
                "seed character identity/ownership differs")
        row = rows[0]
        all_ids.extend((row["id"], row["entity"]))
        expected_characters.append({"account": owner, "id": row["id"], "entity": row["entity"],
                                    "revision": 1, "name": ("Source", "Target")[index], "world": WORLD,
                                    "cell": [1, 2, 3], "offset": [0, 1.5, 1023.75]})
    require(len(set(all_ids)) == len(all_ids) and not set(all_ids) & {WORLD, *ACCOUNTS, identity(3)},
            "seed identities are not distinct")
    expected = {"store": snapshot["store"], "world": WORLD, "schema": 3,
                "accounts": [{"id": owner, "revoked": False, "roster_revision": 2} for owner in ACCOUNTS],
                "certificates": [{"account": owner, "fingerprint": f"{index + 1:064x}", "revoked": False}
                                 for index, owner in enumerate(ACCOUNTS)],
                "characters": sorted(expected_characters, key=lambda row: row["id"]),
                "economies": [],
                "streams": [{"actor": owner, "high_watermark": 0, "receipts": []} for owner in ACCOUNTS]}
    for index, row in enumerate(expected_characters):
        if index != 0 or mutation != "initialize-economy":
            expected["economies"].append({"account": row["account"], "character": row["id"], "revision": 1,
                                          "currency": (1000, 200)[index], "items": (100, 20)[index]})
    expected["economies"].sort(key=lambda row: row["character"])
    if mutation in ("transfer-prune", "transfer-prune-reject"):
        for sequence in range(1, 9):
            apply_transfer(expected, sequence, (1, 1))
    same(snapshot, expected, "fresh seed state differs: " + mutation)


def expected_after(seed, mutation, observed):
    expected = copy.deepcopy(seed)
    if mutation == "provision":
        expected["accounts"].append({"id": identity(3), "revoked": False, "roster_revision": 1})
        expected["streams"].append({"actor": identity(3), "high_watermark": 0, "receipts": []})
    elif mutation == "enroll":
        expected["certificates"].append({"account": ACCOUNTS[0], "fingerprint": f"{3:064x}", "revoked": False})
    elif mutation == "revoke-account":
        expected["accounts"][0]["revoked"] = True
    elif mutation == "revoke-certificate":
        expected["certificates"][0]["revoked"] = True
    elif mutation == "create-character":
        rows = [row for row in observed["characters"] if row.get("name") == "Created"]
        require(len(rows) == 1 and uuid(rows[0].get("id")) and uuid(rows[0].get("entity")),
                "created character missing or invalid")
        created = rows[0]
        used = {seed["store"], WORLD, *ACCOUNTS, identity(3)}
        used.update(value for row in seed["characters"] for value in (row["id"], row["entity"]))
        require(created["id"] != created["entity"] and not {created["id"], created["entity"]} & used,
                "created character reuses an identity")
        expected["characters"].append({"account": ACCOUNTS[0], "id": created["id"],
                                       "entity": created["entity"], "revision": 1, "name": "Created", "world": WORLD,
                                       "cell": [1, 2, 3], "offset": [0, 1.5, 1023.75]})
        expected["characters"].sort(key=lambda row: row["id"])
        expected["accounts"][0]["roster_revision"] = 3
    elif mutation == "save-character":
        source_character(expected).update(revision=2, name="Saved", cell=[9, 2, 3])
    elif mutation == "initialize-economy":
        expected["economies"].append({"account": ACCOUNTS[0], "character": source_character(expected)["id"],
                                      "revision": 1, "currency": 1000, "items": 100})
        expected["economies"].sort(key=lambda row: row["character"])
    else:
        require(mutation.startswith("transfer"), "unknown expected mutation")
        rejected = mutation in ("transfer-reject", "transfer-prune-reject")
        sequence = 9 if mutation in ("transfer-prune", "transfer-prune-reject") else 1
        apply_transfer(expected, sequence, (1001 if rejected else 7, 2), rejected)
    return expected


def validate_outcome(value, mutation, final, *, replayed=False, conflict=False, outage=False, sqlite_code=0):
    require(isinstance(value, dict) and set(value) == {"ok", "code", "operation", "sqlite_code", "replayed", "receipt"}
            and type(value["sqlite_code"]) is int and value["sqlite_code"] == sqlite_code,
            "mutation outcome fields differ")
    if outage or conflict:
        require(value["ok"] is False and value["code"] == ("io_failure" if outage else "conflict")
                and isinstance(value["operation"], str) and value["operation"].startswith("storage.")
                and value["replayed"] is None and value["receipt"] is None,
                "failed operation acknowledged or wrong failure code")
    else:
        expected = {"ok": True, "code": "", "operation": "", "sqlite_code": 0, "replayed": None, "receipt": None}
        if mutation.startswith("transfer"):
            expected.update(replayed=replayed, receipt=final["streams"][0]["receipts"][-1])
        same(value, expected, "successful/replayed outcome differs")


def validate_scenarios(rows):
    require(isinstance(rows, list) and [row.get("name") for row in rows] ==
            [mutation + "." + phase for mutation, phase in RECIPES], "recovery row inventory differs")
    databases, stores, probe = set(), set(), None
    for row, (mutation, phase) in zip(rows, RECIPES):
        label = mutation + "." + phase
        require(row.get("status") == "PASS" and row.get("mutation") == mutation and row.get("phase") == phase,
                "recovery row identity/status differs: " + label)
        processes = row.get("processes")
        require(isinstance(processes, list) and len(processes) == 3, "recovery child inventory differs")
        database = None
        for child, role in zip(processes, ("seed", "mutate", "reconcile")):
            expected_exit = 73 if role == "mutate" and phase not in OUTAGES else 0
            require(child.get("name") == label + "." + role and child.get("status") == "PASS"
                    and type(child.get("exit_code")) is int and child["exit_code"] == expected_exit
                    and child.get("timed_out") is False and child.get("cleanup_errors") == [],
                    "recovery child failed: " + label + "." + role)
            command = child.get("command")
            require(isinstance(command, list) and len(command) == (5 if role == "mutate" else 4)
                    and all(isinstance(part, str) and part for part in command), "invalid recovery command")
            if probe is None:
                probe = command[0]
            if database is None:
                database = command[-1]
                require(database not in databases, "recovery rows reused database path")
                databases.add(database)
            same(command, [probe, role, mutation, *([phase] if role == "mutate" else []), database],
                 "recovery child mode, inputs or path differs")
            lines = child.get("stdout", "").splitlines()
            require(len(lines) == 1, "recovery child must report exactly one JSON record")
            value = json.loads(lines[0])
            same(value, child.get("result"), "recovery parsed result differs from stdout")
            require(isinstance(value, dict) and value.get("status") == "PASS", "recovery child JSON failed")
        seeded, interrupted, recovered = [child["result"] for child in processes]
        require(set(seeded) == {"status", "event", "snapshot"} and seeded["event"] == "seed",
                "recovery seed result differs")
        validate_seed(seeded["snapshot"], mutation)
        seed = seeded["snapshot"]
        require(seed["store"] not in stores, "recovery rows reused fresh store identity")
        stores.add(seed["store"])
        require(set(recovered) == {"status", "event", "before", "retry", "after", "reopened"}
                and recovered["event"] == "reconcile", "reconciliation fields differ")
        committed = phase in ("after_commit", "after_reply")
        final = expected_after(seed, mutation, recovered["after"])
        same(recovered["before"], final if committed else seed, "reopened boundary state differs: " + label)
        same(recovered["after"], final, "retry was not exactly one durable effect: " + label)
        same(recovered["reopened"], final, "second reopen changed durable state: " + label)
        validate_outcome(recovered["retry"], mutation, final,
                         replayed=committed and mutation.startswith("transfer"),
                         conflict=committed and mutation not in ("revoke-account", "revoke-certificate")
                         and not mutation.startswith("transfer"))
        require(interrupted.get("mutation") == mutation and interrupted.get("phase") == phase,
                "interruption boundary differs")
        if phase in OUTAGES:
            require(set(interrupted) == {"status", "event", "mutation", "phase", "injected_calls",
                                         "owner_fenced", "outcome"}
                    and interrupted["event"] == "outage"
                    and type(interrupted["injected_calls"]) is int and interrupted["injected_calls"] > 0
                    and type(interrupted["owner_fenced"]) is bool
                    and (phase != "sync-outage" or interrupted["owner_fenced"]),
                    "I/O outage was not injected or ambiguous commit owner remained active")
            # SQLite extended codes: IOERR_WRITE=778, IOERR_FSYNC=1034, FULL=13.
            validate_outcome(interrupted["outcome"], mutation, final, outage=True,
                             sqlite_code={"write-outage": 778, "sync-outage": 1034, "full-outage": 13}[phase])
        elif phase == "after_reply":
            require(set(interrupted) == {"status", "event", "mutation", "phase", "outcome", "snapshot"}
                    and interrupted["event"] == "ack", "successful reply was not observed")
            same(interrupted["snapshot"], final, "acknowledged state was lost after process exit")
            validate_outcome(interrupted["outcome"], mutation, final)
        else:
            require(set(interrupted) == {"status", "event", "mutation", "phase"}
                    and interrupted["event"] == "boundary", "pre-reply crash boundary was not reached")
        require(isinstance(row.get("database_sha256"), str)
                and bool(re.fullmatch(r"[0-9a-f]{64}", row["database_sha256"])), "recovered database hash missing")


def validate_receipt(receipt):
    require(receipt.get("status") == "PASS" and receipt.get("error") is None, "recovery receipt failed")
    validate_scenarios(receipt.get("rows"))
    controls = receipt.get("supervisor_negative_controls")
    require(isinstance(controls, list) and [row.get("name") for row in controls] == list(NEGATIVES),
            "recovery negative control inventory differs")
    for row in controls:
        require(row.get("status") == "PASS" and type(row.get("exit_code")) is int and row["exit_code"] == 1
                and row.get("reported_status") == "FAIL" and row.get("summary_status") == "FAIL"
                and type(row.get("runner_calls")) is int and row["runner_calls"] == 1
                and isinstance(row.get("error"), str) and bool(row["error"]), "recovery supervisor falsely passed")
        if row["name"] == "empty-exception":
            require(row["error"] == "AssertionError", "empty exception lost failure type")


def execute(name, command, expected_exit, deadline):
    remaining = deadline - time.monotonic()
    require(remaining > 0, "recovery process matrix deadline exceeded")
    result = run_process(command, timeout=min(8, remaining))
    value = None
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError:
        pass
    good = result.returncode == expected_exit and not result.timed_out and not result.cleanup_errors
    good = good and isinstance(value, dict) and value.get("status") == "PASS"
    return {"name": name, "command": command, "exit_code": result.returncode, "timed_out": result.timed_out,
            "cleanup_errors": list(result.cleanup_errors), "stdout": result.stdout, "stderr": result.stderr,
            "result": value, "status": "PASS" if good else "FAIL"}


def run_scenarios(args):
    rows = []
    args.partial_rows = rows
    deadline = time.monotonic() + 200
    with tempfile.TemporaryDirectory(prefix="storage-recovery-", dir=args.output.parent) as temporary:
        work = Path(temporary).resolve()
        for mutation, phase in RECIPES:
            name = mutation + "." + phase
            folder = work / name
            folder.mkdir()
            database = folder / "Durable recovery \u4e16\u754c.db"
            row = {"name": name, "mutation": mutation, "phase": phase, "status": "FAIL", "processes": []}
            rows.append(row)
            for role in ("seed", "mutate", "reconcile"):
                command = [str(args.probe.resolve()), role, mutation, *([phase] if role == "mutate" else []), str(database)]
                child = execute(name + "." + role, command,
                                73 if role == "mutate" and phase not in OUTAGES else 0, deadline)
                row["processes"].append(child)
                require(child["status"] == "PASS", "Recovery child failed: " + json.dumps(child))
            row.update(status="PASS", database_sha256=hashlib.sha256(database.read_bytes()).hexdigest())
    return rows


def negative_controls(args, rows):
    controls = []
    for name in NEGATIVES:
        changed = copy.deepcopy(rows)
        if name == "missing-row": changed.pop()
        elif name == "duplicate-row": changed[1] = copy.deepcopy(changed[0])
        elif name == "wrong-mutation": changed[0]["mutation"] = "enroll"
        elif name == "wrong-phase": changed[0]["phase"] = "after_reply"
        elif name == "wrong-child-mode": changed[0]["processes"][0]["command"][1] = "mutate"
        elif name == "wrong-database": changed[0]["processes"][1]["command"][-1] += ".other"
        elif name == "nonzero-exit": changed[0]["processes"][0]["exit_code"] = 9
        elif name == "timeout": changed[0]["processes"][0]["timed_out"] = True
        elif name == "cleanup-failure": changed[0]["processes"][0]["cleanup_errors"] = ["controlled cleanup failure"]
        elif name == "reused-fresh-seed":
            # Copy a completely valid same-mutation seed and reconciliation, updating command/boundary names.
            changed[1]["processes"][0]["result"] = copy.deepcopy(changed[0]["processes"][0]["result"])
            changed[1]["processes"][2]["result"] = copy.deepcopy(changed[0]["processes"][2]["result"])
            for child in changed[1]["processes"]:
                child["stdout"] = json.dumps(child["result"]) + "\n"
        elif name != "empty-exception":
            selection = ("initialize-economy", "after_reply") if name == "lost-acknowledged-effect" else ("transfer", "after_commit")
            if name == "missing-vfs-failure": selection = ("provision", "write-outage")
            if name == "changed-retention": selection = ("transfer-prune", "after_commit")
            selected = changed[RECIPES.index(selection)]
            child = selected["processes"][1 if name == "missing-vfs-failure" else 2]
            value = child["result"]
            if name == "changed-store": value["before"]["store"] = identity(999)
            elif name == "lost-acknowledged-effect": value["before"] = copy.deepcopy(selected["processes"][0]["result"]["snapshot"])
            elif name == "partial-transfer": economy(value["before"], True)["currency"] -= 7
            elif name == "duplicate-retry-effect": economy(value["after"])["currency"] -= 7
            elif name == "missing-vfs-failure": value["injected_calls"] = 0
            elif name == "changed-retention": value["before"]["streams"][0]["receipts"].pop()
            elif name == "wrong-replay": value["retry"]["replayed"] = False
            child["stdout"] = json.dumps(value) + "\n"
        calls = 0

        def injected(_args):
            nonlocal calls
            calls += 1
            if name == "empty-exception":
                raise AssertionError()
            return changed

        output = args.output.with_name(args.output.stem + ".negative-" + name + ".json")
        capture = io.StringIO()
        with redirect_stdout(capture):
            code = main(["--probe", str(args.probe), "--output", str(output)],
                        scenario_runner=injected, run_controls=False)
        result = json.loads(output.read_text(encoding="utf-8"))
        summary = json.loads(capture.getvalue().splitlines()[-1])
        require(calls == 1 and code == 1 and result["status"] == "FAIL" and summary["status"] == "FAIL"
                and bool(result["error"]) and result["error"] == summary["error"], "Supervisor accepted " + name)
        controls.append({"name": name, "status": "PASS", "exit_code": code, "runner_calls": calls,
                         "reported_status": result["status"], "summary_status": summary["status"], "error": result["error"]})
    return controls


def main(argv=None, *, scenario_runner=run_scenarios, run_controls=True):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    rows, controls, error = [], [], None
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        rows = scenario_runner(args)
        validate_scenarios(rows)
        if run_controls:
            controls = negative_controls(args, rows)
    except Exception as failure:
        error = str(failure) or type(failure).__name__
        rows = getattr(args, "partial_rows", rows)
    receipt = {"status": "FAIL" if error is not None else "PASS", "error": error,
               "rows": rows, "supervisor_negative_controls": controls,
               "scope": "fresh local file per case; trusted Store mutations, real process exit before/after commit/reply, native SQLite xWrite/xSync/FULL fault injection, reopen and same-identity retry; authenticated runtime fencing is separately qualified; no physical power-loss or migration claim"}
    if error is None and run_controls:
        try:
            validate_receipt(receipt)
        except Exception as failure:
            error = str(failure) or type(failure).__name__
            receipt.update(status="FAIL", error=error)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": receipt["status"], "rows": len(rows),
                      "processes": sum(len(row["processes"]) for row in rows),
                      "negative_controls": len(controls), "error": error}))
    return 1 if error is not None else 0


if __name__ == "__main__":
    raise SystemExit(main())
