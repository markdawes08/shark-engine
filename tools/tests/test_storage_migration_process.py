"""Real-file sequential migration, backup and isolated restore interruption matrix."""
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

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/storage"
RECIPES = []
for version in (1, 2):
    phases = (["after_v1_to_v2"] if version == 1 else []) + [
        "after_v2_to_v3", "before_migration_commit", "after_migration_commit", "after_reply"]
    for kind in ("crash", "fail"):
        RECIPES.extend(("migrate", version, phase, kind) for phase in phases
                       if kind == "crash" or phase != "after_reply")
for operation in ("backup", "restore"):
    for version in (1, 2, 3):
        for kind in ("crash", "fail"):
            RECIPES.extend((operation, version, phase, kind) for phase in
                           (operation + "_step", operation + "_complete", operation + "_validated", "after_reply")
                           if kind == "crash" or phase != "after_reply")
RECIPES.extend((("backup", 3, "future", "reject"), ("restore", 3, "foreign", "reject"),
                ("migrate", 2, "extra-schema", "reject"), ("migrate", 2, "missing-backup", "reject"),
                ("migrate", 2, "mismatched-backup", "reject"), ("migrate", 2, "corrupt-backup", "reject")))
RECIPES.extend(("migrate", version, "main_write", "crash") for version in (1, 2))
RECIPES = tuple(RECIPES)
NEGATIVES = ("missing-row", "duplicate-row", "wrong-phase", "wrong-child-mode", "wrong-database",
             "nonzero-exit", "timeout", "cleanup-failure", "stdout-mismatch", "missing-hook",
             "lost-acknowledged-migration", "changed-store", "changed-registry", "changed-receipt",
             "reset-high-water", "missing-replay-check", "source-overwrite", "backup-overwrite",
             "destination-overwrite", "missing-native-write", "missing-hot-journal", "hot-inspection-mutated", "completed-copy-rejected", "empty-exception")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def same(actual, expected, message):
    require(json.dumps(actual, sort_keys=True) == json.dumps(expected, sort_keys=True), message)


def identity(value):
    return f"00000000-0000-4000-8000-{value:012x}"


STORE, WORLD = identity(700), identity(900)


def integer(value): return {"integer": value}
def text(value): return {"text": value}
def real(value): return {"real": int(value) if int(value) == value else value}
def blob(value): return {"blob": value}


def fixture_hashes():
    return {f"schema-{version}.sql": hashlib.sha256((FIXTURES / f"schema-{version}.sql").read_bytes()).hexdigest()
            for version in (1, 2, 3)}


def expected_snapshot(version, *, migrated_from=None):
    schema = []
    for line in (FIXTURES / f"schema-{version}.sql").read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"CREATE TABLE (shark_[a-z_]+)\(.*;", line)
        if match:
            name = match[1]
            schema.append([text("table"), text(name), text(name), text(line[:-1])])
    require(len(schema) == {1: 1, 2: 4, 3: 6}[version], "independent fixture schema inventory differs")
    for table, count in (("accounts", 1), ("certificates", 1), ("characters", 2),
                         ("economy", 1), ("transfer_receipts", 1)):
        if version < (3 if table in ("economy", "transfer_receipts") else 2):
            continue
        for number in range(1, count + 1):
            schema.append([text("index"), text(f"sqlite_autoindex_shark_{table}_{number}"),
                           text("shark_" + table), None])
    schema.sort(key=lambda row: row[1]["text"])
    result = {"version": version, "schema": schema, "header": [[integer(1397246788)]],
              "user_version": [[integer(version)]], "store": [[integer(1), text(STORE), text(WORLD), integer(version)]]}
    if version == 1:
        return result
    result.update(accounts=[], certificates=[], characters=[])
    if migrated_from != 1:
        result["accounts"] = [[text(identity(owner)), integer(int(owner == 3)), integer(roster)]
                              for owner, roster in ((1, 6), (2, 4), (3, 2))]
        certs = [(1, "96baafc1e5c4ef1173b8d8c0ffa4916b39c633cc3b88b100f86fd48b9810fb20", 0),
                 (2, "95e15d6eab226755438ab548b7bfd23a3e9362c945ac3c0a8814b26bc441520b", 0),
                 (1, f"{2:064x}", 1), (3, f"{4:064x}", 0)]
        result["certificates"] = [[blob(fingerprint), text(identity(owner)), integer(revoked)]
                                  for owner, fingerprint, revoked in sorted(certs, key=lambda row: row[1])]
        characters = [(1, 7, "Vanguard", [-3, 2, 2147483647], [0.25, 512.5, 1023.75]),
                      (2, 11, "Archivist", [-2147483648, 0, 4], [999.125, 0, 10]),
                      (3, 3, "Dormant", [1, 2, 3], [0, 1.5, 0])]
        result["characters"] = [[text(identity(owner + 100)), text(identity(owner)), text(identity(owner + 200)),
                                  integer(revision), text(name), *map(integer, cells), *map(real, offsets)]
                                 for owner, revision, name, cells, offsets in characters]
    if version == 2:
        return result
    for index, row in enumerate(result["accounts"]):
        row.append(integer(9 if migrated_from is None and index == 0 else 0))
    result.update(economies=[], receipts=[])
    if migrated_from is None:
        result["economies"] = [[text(identity(owner + 100)), integer(3), integer(currency), integer(items)]
                               for owner, currency, items in ((1, 90, 17), (2, 110, 23))]
        for sequence in range(2, 10):
            values = [2, 2, 3, 1, 0, 3, 90, 17, 3, 110, 23] if sequence == 9 else [1, 1, 7, 2, 1, 2, 93, 18, 2, 107, 22]
            result["receipts"].append([text(identity(1)), integer(sequence), text(identity(101)), text(identity(102)),
                                       *map(integer, values)])
    return result


def expected_rejected(version, phase):
    source, backup = expected_snapshot(version), expected_snapshot(version)
    if phase == "future": source["user_version"] = [[integer(4)]]
    elif phase == "foreign": source["header"] = [[integer(17)]]
    elif phase == "extra-schema":
        source["schema"].append([text("table"), text("unexpected"), text("unexpected"),
                                  text("CREATE TABLE unexpected(value INTEGER) STRICT")])
        source["schema"].sort(key=lambda row: row[1]["text"])
    elif phase == "mismatched-backup": backup["characters"][0][4] = text("Changed backup")
    elif phase in ("missing-backup", "corrupt-backup"): backup = None
    else: raise AssertionError("unknown rejection phase")
    return source, backup


def metadata(version):
    return {"store": STORE, "world": WORLD, "version": version}


def migration(value, before, current):
    same(value, {"before": metadata(before), "after": metadata(3), "already_current": current},
         "migration result/identity differs")


def info(value, version):
    require(isinstance(value, dict) and set(value) == {"metadata", "bytes", "accounts", "certificates",
            "characters", "economies", "operation_streams", "receipts"}, "snapshot info fields differ")
    same(value["metadata"], metadata(version), "snapshot info identity differs")
    require(type(value["bytes"]) is int and 4096 <= value["bytes"] <= 16 * 1024 * 1024
            and value["bytes"] % 4096 == 0, "snapshot byte bound differs")
    expected = {"accounts": 0 if version == 1 else 3, "certificates": 0 if version == 1 else 4,
                "characters": 0 if version == 1 else 3, "economies": 2 if version == 3 else 0,
                "operation_streams": 3 if version == 3 else 0, "receipts": 8 if version == 3 else 0}
    same({key: value[key] for key in expected}, expected, "snapshot counts differ")


def failed(value, *, conflict=False):
    require(isinstance(value, dict) and set(value) == {"code", "operation"}
            and value["code"] in (("conflict",) if conflict else ("conflict", "io_failure", "invalid_configuration", "execution_failed"))
            and isinstance(value["operation"], str) and value["operation"].startswith("storage."),
            "missing or unexpected owned maintenance error")


def digest(value, *, absent=False):
    require((absent and value is None) or isinstance(value, str) and bool(re.fullmatch(r"[0-9a-f]{64}", value)),
            "artifact hash missing or invalid")


def validate_scenarios(rows):
    require(isinstance(rows, list) and len(rows) == len(RECIPES), "maintenance row inventory differs")
    paths = set()
    for row, (operation, version, phase, kind) in zip(rows, RECIPES):
        name = f"{operation}.v{version}.{phase}.{kind}"
        require(row.get("name") == name and row.get("operation") == operation and type(row.get("version")) is int and row["version"] == version
                and row.get("phase") == phase and row.get("kind") == kind and row.get("status") == "PASS",
                "maintenance row identity differs")
        children = row.get("processes")
        modes = ("seed", "operate", "inspect-hot", "reconcile") if phase == "main_write" else ("seed", "operate", "reconcile")
        require(isinstance(children, list) and len(children) == len(modes), "maintenance child inventory differs")
        commands = []
        for child, mode in zip(children, modes):
            command = child.get("command")
            require(isinstance(command, list) and len(command) == 10 and all(isinstance(item, str) for item in command)
                    and command[1:6] == [mode, operation, str(version), phase, kind], "maintenance child command differs")
            commands.append(command)
            code = 73 if mode == "operate" and kind == "crash" else 0
            require(child.get("name") == name + "." + mode and child.get("status") == "PASS"
                    and type(child.get("exit_code")) is int and child["exit_code"] == code
                    and child.get("timed_out") is False and child.get("cleanup_errors") == []
                    and child.get("stderr") == "", "maintenance child did not finish cleanly")
            same(json.loads(child["stdout"]), child.get("result"), "maintenance stdout/result mismatch")
            require(child["result"].get("status") == "PASS", "maintenance child reported failure")
        require(all(command[0] == commands[0][0] and command[6:] == commands[0][6:] for command in commands),
                "maintenance paths changed between children")
        require(commands[0][7] not in paths, "maintenance reused scenario source")
        paths.add(commands[0][7])
        source_path, destination_path, retry_path = map(Path, commands[0][7:])
        require(len({str(path.parent) for path in (source_path, destination_path, retry_path)}) == 3,
                "maintenance did not use separate destination directories")
        seed, action, final = children[0]["result"], children[1]["result"], children[-1]["result"]
        source = expected_snapshot(version)
        backup = source if operation == "migrate" else None
        if kind == "reject":
            source, backup = expected_rejected(version, phase)
            if operation != "migrate": backup = None
        same(seed, {"status": "PASS", "event": "seed", "source": source, "backup": backup},
             "independent seeded schema/data differ")
        if phase == "main_write":
            require(set(action) == {"status", "event", "operation", "phase", "hook_calls", "sqlite_code", "amount",
                                   "offset", "journal_bytes", "journal_header"}
                    and action["event"] == "native-write" and action["operation"] == "migrate" and action["phase"] == phase
                    and type(action["hook_calls"]) is int and action["hook_calls"] == 1
                    and type(action["sqlite_code"]) is int and action["sqlite_code"] == 0
                    and type(action["amount"]) is int and action["amount"] > 0
                    and type(action["offset"]) is int and action["offset"] >= 0
                    and type(action["journal_bytes"]) is int and 512 < action["journal_bytes"] <= 32 * 1024 * 1024
                    and action["journal_header"] == "d9d505f920a163d7", "actual database write/hot-journal evidence missing")
        elif kind == "crash" and phase != "after_reply":
            same(action, {"status": "PASS", "event": "boundary", "operation": operation,
                           "phase": phase, "hook_calls": 1}, "requested crash boundary was not reached")
        else:
            require(set(action) == {"status", "event", "operation", "phase", "kind", "ok", "hook_calls", "result"}
                    and action["event"] == "operation" and action["operation"] == operation and action["phase"] == phase
                    and action["kind"] == kind and action["ok"] is (phase == "after_reply")
                    and type(action["hook_calls"]) is int
                    and action["hook_calls"] == (1 if kind == "fail" else 0), "maintenance result/hook differs")
            if phase == "after_reply":
                if operation == "migrate": migration(action["result"], version, False)
                else: info(action["result"], version)
            else:
                failed(action["result"])
        hashes = row.get("hashes")
        expected_hashes = {"seed", "operated", "reconciled"} | ({"inspected"} if phase == "main_write" else set())
        require(isinstance(hashes, dict) and set(hashes) == expected_hashes, "maintenance hash phases differ")
        for values in hashes.values():
            require(set(values) == {"source", "destination", "retry"}, "maintenance hash file inventory differs")
            for key, value in values.items(): digest(value, absent=key != "source")
        if kind == "reject":
            same(final, {"status": "PASS", "event": "rejected-preserved", "source": source}, "rejection changed source records")
            same(hashes["seed"], hashes["operated"], "rejected maintenance changed file bytes")
            same(hashes["seed"], hashes["reconciled"], "rejected maintenance changed files on reopen")
        elif operation == "migrate":
            require(set(final) == {"status", "event", "migration", "repeated", "source", "backup"}
                    and final["event"] == "reconcile", "migration reconciliation fields differ")
            committed = phase in ("after_migration_commit", "after_reply")
            migration(final["migration"], 3 if committed else version, committed)
            migration(final["repeated"], 3, True)
            same(final["source"], expected_snapshot(3, migrated_from=version), "migration lost or granted durable state")
            same(final["backup"], backup, "pre-upgrade backup changed")
            require(hashes["seed"]["destination"] == hashes["operated"]["destination"] == hashes["reconciled"]["destination"],
                    "migration overwrote pre-upgrade backup bytes")
            require(all(values["retry"] is None for values in hashes.values()), "migration unexpectedly created retry destination")
            if phase == "main_write":
                require(hashes["seed"]["source"] != hashes["operated"]["source"],
                        "native write crash did not actually change database bytes")
                same(hashes["operated"], hashes["inspected"], "read-only hot inspection changed database bytes")
                hot = children[2]["result"]
                require(set(hot) == {"status", "event", "error"} and hot["event"] == "hot-rejected",
                        "hot-journal read-only inspection was not rejected")
                failed(hot["error"])
                journal = row.get("hot_journal")
                require(isinstance(journal, dict) and set(journal) == {"before_inspection", "after_inspection"},
                        "hot journal hash evidence missing")
                digest(journal["before_inspection"])
                require(journal["before_inspection"] == journal["after_inspection"],
                        "read-only hot inspection changed journal bytes")
        else:
            require(set(final) == {"status", "event", "destination_valid", "destination", "existing_retry", "fresh_copy",
                    "source", "retry", "economic_replays_checked"} and final["event"] == "reconcile"
                    and type(final["destination_valid"]) is bool, "copy reconciliation fields differ")
            if final["destination_valid"]:
                same(final["destination"], source, "copy admitted incomplete destination")
            else:
                require(phase.endswith("_step"), "completed copy failed independent validation")
                failed(final["destination"])
            failed(final["existing_retry"], conflict=True)
            info(final["fresh_copy"], version)
            same(final["source"], source, "copy changed source state")
            same(final["retry"], source, "fresh retry did not preserve complete snapshot")
            require(final["economic_replays_checked"] is (version == 3), "restored economic retry/expiry checks missing")
            require(hashes["seed"]["source"] == hashes["operated"]["source"] == hashes["reconciled"]["source"],
                    "copy changed source bytes")
            require(hashes["seed"]["destination"] is None and hashes["operated"]["destination"] is not None
                    and hashes["operated"]["destination"] == hashes["reconciled"]["destination"], "copy retry overwrote allocated destination")
            require(hashes["seed"]["retry"] is None and hashes["operated"]["retry"] is None
                    and hashes["reconciled"]["retry"] is not None, "fresh copy destination was not newly created")


def validate_receipt(receipt):
    require(receipt.get("status") == "PASS" and receipt.get("error") is None, "maintenance receipt failed")
    same(receipt.get("fixture_sources"), fixture_hashes(), "maintenance independent fixture binding differs")
    validate_scenarios(receipt.get("rows"))
    controls = receipt.get("supervisor_negative_controls")
    require(isinstance(controls, list) and [row.get("name") for row in controls] == list(NEGATIVES), "maintenance controls inventory differs")
    for row in controls:
        require(row.get("status") == "PASS" and type(row.get("exit_code")) is int and row["exit_code"] == 1
                and row.get("reported_status") == "FAIL" and row.get("summary_status") == "FAIL"
                and type(row.get("runner_calls")) is int and row["runner_calls"] == 1
                and isinstance(row.get("error"), str) and bool(row["error"]), "maintenance false-PASS control failed")
        if row["name"] == "empty-exception": require(row["error"] == "AssertionError", "empty exception lost its type")


def execute(name, command, expected_exit, deadline):
    remaining = deadline - time.monotonic()
    require(remaining > 0, "maintenance matrix deadline exceeded")
    result = run_process(command, timeout=min(10, remaining))
    value = None
    try: value = json.loads(result.stdout)
    except json.JSONDecodeError: pass
    good = result.returncode == expected_exit and not result.timed_out and not result.cleanup_errors
    good = good and isinstance(value, dict) and value.get("status") == "PASS" and result.stderr == ""
    return {"name": name, "command": command, "exit_code": result.returncode, "timed_out": result.timed_out,
            "cleanup_errors": list(result.cleanup_errors), "stdout": result.stdout, "stderr": result.stderr,
            "result": value, "status": "PASS" if good else "FAIL"}


def run_scenarios(args):
    rows = []
    args.partial_rows = rows
    require(args.fixtures.resolve() == FIXTURES.resolve(), "fixture directory is not the reviewed source fixture")
    deadline = time.monotonic() + 210
    with tempfile.TemporaryDirectory(prefix="storage-maintenance-", dir=args.output.parent) as temporary:
        work = Path(temporary).resolve()
        for operation, version, phase, kind in RECIPES:
            name = f"{operation}.v{version}.{phase}.{kind}"
            folder = work / name
            paths = []
            for leaf in ("source", "destination", "retry"):
                directory = folder / leaf
                directory.mkdir(parents=True)
                paths.append(directory / "Durable maintenance \u4e16\u754c.db")
            row = {"name": name, "operation": operation, "version": version, "phase": phase, "kind": kind,
                   "status": "FAIL", "processes": [], "hashes": {}}
            rows.append(row)
            roles = [("seed", "seed"), ("operate", "operated")]
            if phase == "main_write": roles.append(("inspect-hot", "inspected"))
            roles.append(("reconcile", "reconciled"))
            for role, hash_phase in roles:
                command = [str(args.probe.resolve()), role, operation, str(version), phase, kind,
                           str(args.fixtures.resolve()), *map(str, paths)]
                child = execute(name + "." + role, command, 73 if role == "operate" and kind == "crash" else 0, deadline)
                row["processes"].append(child)
                require(child["status"] == "PASS", "Maintenance child failed: " + json.dumps(child))
                row["hashes"][hash_phase] = {key: hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
                                              for key, path in zip(("source", "destination", "retry"), paths)}
                if phase == "main_write" and role in ("operate", "inspect-hot"):
                    row.setdefault("hot_journal", {})["before_inspection" if role == "operate" else "after_inspection"] = \
                        hashlib.sha256(Path(str(paths[0]) + "-journal").read_bytes()).hexdigest()
            row["status"] = "PASS"
    return rows


def negative_controls(args, rows):
    controls = []
    for name in NEGATIVES:
        changed = copy.deepcopy(rows)
        if name == "missing-row": changed.pop()
        elif name == "duplicate-row": changed[1] = copy.deepcopy(changed[0])
        elif name == "wrong-phase": changed[0]["phase"] = "after_reply"
        elif name == "wrong-child-mode": changed[0]["processes"][0]["command"][1] = "operate"
        elif name == "wrong-database": changed[0]["processes"][1]["command"][-2] += ".other"
        elif name == "nonzero-exit": changed[0]["processes"][0]["exit_code"] = 9
        elif name == "timeout": changed[0]["processes"][0]["timed_out"] = True
        elif name == "cleanup-failure": changed[0]["processes"][0]["cleanup_errors"] = ["controlled cleanup failure"]
        elif name == "stdout-mismatch": changed[0]["processes"][0]["stdout"] = "{}"
        elif name == "source-overwrite": changed[RECIPES.index(("migrate", 2, "corrupt-backup", "reject"))]["hashes"]["operated"]["source"] = "0" * 64
        elif name == "backup-overwrite": changed[0]["hashes"]["operated"]["destination"] = "0" * 64
        elif name == "destination-overwrite":
            selected = changed[RECIPES.index(("backup", 3, "after_reply", "crash"))]
            selected["hashes"]["reconciled"]["destination"] = "0" * 64
        elif name == "hot-inspection-mutated":
            changed[RECIPES.index(("migrate", 1, "main_write", "crash"))]["hot_journal"]["after_inspection"] = "0" * 64
        elif name == "completed-copy-rejected":
            child = changed[RECIPES.index(("backup", 3, "backup_complete", "crash"))]["processes"][2]
            child["result"]["destination_valid"] = False
            child["result"]["destination"] = {"code": "io_failure", "operation": "storage.inspect_snapshot"}
            child["stdout"] = json.dumps(child["result"]) + "\n"
        elif name != "empty-exception":
            index = RECIPES.index(("restore", 3, "after_reply", "crash"))
            if name == "missing-hook": index = 0
            elif name == "lost-acknowledged-migration": index = RECIPES.index(("migrate", 1, "after_reply", "crash"))
            elif name in ("missing-native-write", "missing-hot-journal"):
                index = RECIPES.index(("migrate", 1, "main_write", "crash"))
            child = changed[index]["processes"][1 if name in ("missing-hook", "missing-native-write", "missing-hot-journal") else 2]
            value = child["result"]
            if name == "missing-hook": value["hook_calls"] = 0
            elif name == "missing-native-write": value["amount"] = 0
            elif name == "missing-hot-journal": value["journal_header"] = "0000000000000000"
            elif name == "lost-acknowledged-migration": value["migration"]["already_current"] = False
            elif name == "changed-store": value["retry"]["store"][0][1] = text(identity(999))
            elif name == "changed-registry": value["retry"]["characters"][0][4] = text("Lost checkpoint")
            elif name == "changed-receipt": value["retry"]["receipts"][-1][9] = integer(89)
            elif name == "reset-high-water": value["retry"]["accounts"][0][3] = integer(0)
            elif name == "missing-replay-check": value["economic_replays_checked"] = False
            child["stdout"] = json.dumps(value) + "\n"
        calls = 0

        def injected(_args):
            nonlocal calls
            calls += 1
            if name == "empty-exception": raise AssertionError()
            return changed

        output = args.output.with_name(args.output.stem + ".negative-" + name + ".json")
        capture = io.StringIO()
        with redirect_stdout(capture):
            code = main(["--probe", str(args.probe), "--fixtures", str(args.fixtures), "--output", str(output)],
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
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    rows, controls, error = [], [], None
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        rows = scenario_runner(args)
        validate_scenarios(rows)
        if run_controls: controls = negative_controls(args, rows)
    except Exception as failure:
        error = str(failure) or type(failure).__name__
        rows = getattr(args, "partial_rows", rows)
    receipt = {"status": "FAIL" if error is not None else "PASS", "error": error,
               "fixture_sources": fixture_hashes(), "rows": rows, "supervisor_negative_controls": controls,
               "scope": "Independent historical/current SQL; offline sequential migration, validated pre-upgrade backup, create-only separate-directory restore, actual process exits/injected maintenance failures and exact durable-state retry; no active-store replacement or physical host/disk-loss claim"}
    if error is None and run_controls:
        try: validate_receipt(receipt)
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
