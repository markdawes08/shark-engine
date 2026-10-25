"""Verify lost authoring replies and explicit reconciliation across real processes."""
from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process


def validate_report(mode: str, report: dict) -> None:
    common = {"status", "phase", "scope", "high_watermark", "history_entries", "name"}
    assert set(report) == common | ({"old_scope", "query", "retry"} if mode == "recover" else set())
    assert report["status"] == "PASS" and report["phase"] == mode
    scope = uuid.UUID(report["scope"])
    assert scope.version == 4 and str(scope) == report["scope"]
    expected = {"produce": (1, 1, "Original lost reply"),
                "recover": (1, 2, "Reconciled new intent"),
                "verify": (0, 0, "Reconciled new intent")}[mode]
    assert type(report["high_watermark"]) is int and type(report["history_entries"]) is int
    assert (report["high_watermark"], report["history_entries"], report["name"]) == expected
    if mode == "recover":
        assert str(uuid.UUID(report["old_scope"])) == report["old_scope"]
        assert report["old_scope"] != report["scope"]
        assert report["query"] == report["retry"] == "unknown_outcome"


def validate_receipt(receipt: dict) -> None:
    assert receipt["checkpoint"] == "09d" and receipt["status"] == "PASS"
    assert receipt["error"] is None and receipt["seed"] == 1337
    assert receipt["negative_controls"] == ["status", "phase", "scope", "scope", "query", "retry",
                                             "high_watermark", "history_entries", "name"]
    rows = receipt["rows"]
    assert [row["phase"] for row in rows] == ["produce", "recover", "verify"]
    for row in rows:
        assert row["status"] == "PASS" and type(row["exit_code"]) is int and row["exit_code"] == 0
        assert row["timed_out"] is False and row["cleanup_errors"] == []
        assert len(row["command"]) == 3 and row["command"][1] == row["phase"]
        assert json.loads(row["stdout"]) == row["report"]
        assert len(row["scene_sha256"]) == 64 and int(row["scene_sha256"], 16) >= 0
        validate_report(row["phase"], row["report"])
    assert len({row["report"]["scope"] for row in rows}) == 3
    assert rows[1]["report"]["old_scope"] == rows[0]["report"]["scope"]
    assert rows[0]["scene_sha256"] != rows[1]["scene_sha256"] == rows[2]["scene_sha256"]


def main() -> int:
    if not __debug__:
        raise RuntimeError("Receipt validation requires assertions")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rng-seed", type=int, choices=(1337,), required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows, controls = [], []
    error = None
    try:
        with tempfile.TemporaryDirectory(prefix="authoring-retry-", dir=args.output.parent) as temporary:
            root = Path(temporary)
            before = None
            for mode in ("produce", "recover", "verify"):
                command = [str(args.probe.resolve()), mode, str(root.resolve())]
                result = run_process(command, timeout=20)
                row = {"phase": mode, "command": command, "exit_code": result.returncode,
                       "timed_out": result.timed_out, "cleanup_errors": list(result.cleanup_errors),
                       "stdout": result.stdout, "stderr": result.stderr, "status": "FAIL"}
                rows.append(row)
                assert result.returncode == 0 and not result.timed_out and not result.cleanup_errors
                report = json.loads(result.stdout)
                validate_report(mode, report)
                row["report"] = report
                raw = (root / "scene.sharkscene").read_bytes()
                document = json.loads(raw)
                # Read actual persisted source, independently of the child's receipt.
                assert document["entities"][0]["components"]["shark.name"]["value"] == report["name"]
                row["scene_sha256"] = hashlib.sha256(raw).hexdigest()
                if mode == "produce":
                    before = raw
                    intent = (root / "lost-intent.txt").read_text().splitlines()
                    assert intent == ["retry-fixture-1", report["scope"], "1"]
                elif mode == "recover":
                    assert raw != before and report["old_scope"] == rows[0]["report"]["scope"]
                    before = raw
                else:
                    assert raw == before
                row["status"] = "PASS"
            assert len({row["report"]["scope"] for row in rows}) == 3
            good = rows[1]["report"]
            for key, value in (("status", "FAIL"), ("phase", "produce"), ("scope", good["old_scope"]),
                               ("scope", "invalid"), ("query", "not_recorded"), ("retry", "recorded"),
                               ("high_watermark", True), ("history_entries", 3), ("name", "Original lost reply")):
                bad = deepcopy(good)
                bad[key] = value
                try:
                    validate_report("recover", bad)
                except (AssertionError, ValueError):
                    controls.append(key)
                else:
                    raise AssertionError("Invalid receipt accepted: " + key)
    except Exception as failure:
        error = str(failure) or type(failure).__name__
    summary = {"checkpoint": "09d", "status": "PASS" if error is None else "FAIL",
               "seed": args.rng_seed, "rows": rows, "negative_controls": controls, "error": error}
    if error is None:
        validate_receipt(summary)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary))
    return 0 if error is None else 1


if __name__ == "__main__":
    raise SystemExit(main())
