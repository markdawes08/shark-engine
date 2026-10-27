"""Interrupt real background imports and reconcile their two-asset cache closure."""

from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process


PHASES = ("running", "ready", "staged", "written", "flushed", "before_publish", "published")
STAGED_PHASES = {"staged", "written", "flushed", "before_publish"}
EXPECTED_FIELDS = {
    "format", "schema_version", "status", "state", "root_payload", "leaf_payload",
    "fresh", "recovered", "key", "dependency_key", "owner_scope", "old_job_rejected",
    "jobs_retained", "nodes", "imported", "reused", "changed", "job_sequence",
    "artifacts", "unpublished_stage",
}


def canonical_scope(text):
    value = uuid.UUID(text)
    return value.version == 4 and value.variant == uuid.RFC_4122 and str(value) == text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gltf", action="store_true",
                        help="Qualify actual glTF payloads at ready/pre/post-publication boundaries")
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    probe = args.probe.resolve()
    rows = []
    result = {"format": "shark.asset-jobs-process-supervisor", "schema_version": 1,
              "status": "FAIL", "records": rows}
    phases = ("ready", "before_publish", "published") if args.gltf else PHASES
    root_payload = "StaticMesh" if args.gltf else "Root"
    before_payload, after_payload = ("1", "2") if args.gltf else ("Before", "After")
    if args.gltf:
        result["fixture"] = "gltf"

    def call(name, mode, root, *extra, expected=0):
        process = run_process([str(probe), ("gltf-" if args.gltf else "") + mode,
                               str(root), *extra], timeout=20)
        ok = (process.returncode == expected and not process.timed_out and
              not process.cleanup_errors)
        row = {"name": name, "status": "PASS" if ok else "FAIL",
               "exit_code": process.returncode, "timed_out": process.timed_out,
               "cleanup_errors": list(process.cleanup_errors),
               "stdout": process.stdout, "stderr": process.stderr}
        rows.append(row)
        assert ok, (name, row)
        assert len(process.stdout.encode("utf-8")) <= 4096, name
        assert not process.stderr.strip(), row
        if expected == 77:
            assert not process.stdout.strip(), row
            return None
        value = json.loads(process.stdout)
        assert set(value) == EXPECTED_FIELDS, (name, value)
        assert value["format"] == "shark.asset-jobs-process"
        assert type(value["schema_version"]) is int and value["schema_version"] == 1
        assert value["status"] == "PASS" and value["jobs_retained"] == 0
        assert canonical_scope(value["owner_scope"]), (name, value)
        assert all(re.fullmatch(r"[0-9a-f]{64}", value[field]) for field in ("key", "dependency_key"))
        assert all(type(value[field]) is int and 0 <= value[field] <= 2
                   for field in ("recovered", "nodes", "imported", "reused", "job_sequence"))
        assert all(type(value[field]) is bool
                   for field in ("fresh", "old_job_rejected", "changed", "unpublished_stage"))
        assert type(value["artifacts"]) is int and value["artifacts"] in (2, 4)
        return value

    def check(name, condition):
        rows.append({"name": name, "status": "PASS" if condition else "FAIL"})
        assert condition, name

    try:
        with tempfile.TemporaryDirectory(prefix="shark-jobs-process-", dir=output.parent) as temporary:
            parent = Path(temporary)
            for phase in phases:
                root = parent / (phase + "-caf\u00e9 \u4e16\u754c")
                root.mkdir()
                first = call(phase + "-seed", "seed-jobs", root)
                check(phase + "-seed-closure", first["state"] == "fresh" and first["fresh"] and
                      first["root_payload"] == root_payload and first["leaf_payload"] == before_payload and
                      first["nodes"] == first["imported"] == 2 and first["reused"] == 0 and
                      first["changed"] and first["job_sequence"] == 1 and not first["old_job_rejected"] and
                      first["artifacts"] == 2 and not first["unpublished_stage"])
                cache = root / ".shark-development-cache"
                before = cache.read_bytes()
                root_source = (root / "root.bin").read_bytes()
                if args.gltf:
                    check(phase + "-original-geometry", (root / "leaf.bin").read_bytes() ==
                          struct.pack("<9f", 0, 0, 0, 1, 0, 0, 2, 1, 0))
                sidecars = {name: (root / name).read_bytes()
                            for name in ("root.bin.sharkasset", "leaf.bin.sharkasset")}
                source_stage = root / ".shark-stage-10000000-0000-4000-8000-000000000001"
                source_stage.write_bytes(b"authored recovery evidence")
                call(phase + "-interrupt", "interrupt-jobs", root, phase, expected=77)
                marker = root / "job-marker" / "identity.txt"
                marker_bytes = marker.read_bytes()
                marker_lines = marker_bytes.decode("utf-8").splitlines()
                check(phase + "-observation-marker", len(marker_bytes) <= 128 and
                      len(marker_lines) == 2 and canonical_scope(marker_lines[0]) and
                      marker_lines[0] != first["owner_scope"] and marker_lines[1] == "1" and
                      marker_bytes.endswith(b"\n"))
                committed = phase == "published"
                observed = call(phase + "-reopen", "inspect-after-jobs" if committed else
                                "inspect-before-jobs", root)
                check(phase + "-atomic-cache", (cache.read_bytes() != before) == committed and
                      observed["root_payload"] == root_payload and
                      observed["leaf_payload"] == (after_payload if committed else before_payload) and
                      observed["fresh"] == committed and
                      (observed["key"] != first["key"]) == committed and
                      (observed["dependency_key"] != first["dependency_key"]) == committed)
                check(phase + "-source-preserved", all((root / name).read_bytes() == data
                      for name, data in sidecars.items()) and
                      (root / "root.bin").read_bytes() == (root_source if args.gltf else b"Root") and
                      (root / "leaf.bin").read_bytes() ==
                      (struct.pack("<9f", 0, 0, 0, 2, 0, 0, 2, 1, 0) if args.gltf else b"After"))
                stages = list(root.glob(".shark-cache-stage-*"))
                check(phase + "-stage-count", len(stages) == (1 if phase in STAGED_PHASES else 0) and
                      observed["unpublished_stage"] == (phase in STAGED_PHASES) and
                      observed["artifacts"] == (4 if committed else 2))
                if phase in STAGED_PHASES:
                    blocked = call(phase + "-orphan-admission", "blocked-jobs", root)
                    check(phase + "-orphan-retains-last-valid", blocked["state"] == "blocked" and
                          blocked["old_job_rejected"] and not blocked["changed"] and
                          blocked["artifacts"] == 2 and blocked["unpublished_stage"] and
                          blocked["key"] == first["key"] and cache.read_bytes() == before and
                          list(root.glob(".shark-cache-stage-*")) == stages)
                recovered = call(phase + "-recover", "recover-jobs", root)
                check(phase + "-recovery-result", recovered["recovered"] ==
                      (1 if phase in STAGED_PHASES else 0) and recovered["leaf_payload"] == after_payload and
                      recovered["root_payload"] == root_payload and recovered["fresh"] and
                      recovered["key"] != first["key"] and
                      recovered["dependency_key"] != first["dependency_key"] and
                      recovered["changed"] != committed and recovered["nodes"] == 2 and
                      recovered["imported"] == (0 if committed else 2) and
                      recovered["reused"] == (2 if committed else 0) and recovered["artifacts"] == 4)
                check(phase + "-old-job-rejected", observed["old_job_rejected"] and
                      recovered["old_job_rejected"] and
                      observed["owner_scope"] != marker_lines[0] and
                      recovered["owner_scope"] not in (marker_lines[0], observed["owner_scope"]) and
                      recovered["job_sequence"] == 1)
                check(phase + "-recovery-cleanup", not list(root.glob(".shark-cache-stage-*")) and
                      not recovered["unpublished_stage"] and
                      source_stage.read_bytes() == b"authored recovery evidence" and
                      all((root / name).read_bytes() == data for name, data in sidecars.items()))
                repeated = call(phase + "-repeat-recovery", "recover-jobs", root)
                check(phase + "-idempotent-recovery", repeated["recovered"] == 0 and
                      repeated["key"] == recovered["key"] and
                      repeated["dependency_key"] == recovered["dependency_key"] and
                      repeated["fresh"] and not repeated["changed"] and
                      repeated["old_job_rejected"] and repeated["imported"] == 0 and
                      repeated["reused"] == 2 and repeated["job_sequence"] == 1)
                check(phase + "-marker-preserved", marker.read_bytes() == marker_bytes and
                      repeated["owner_scope"] not in (marker_lines[0], recovered["owner_scope"]))
                result.setdefault("final_snapshots", []).append({
                    "phase": phase, "sha256": hashlib.sha256(cache.read_bytes()).hexdigest(),
                    "bytes": cache.stat().st_size, "key": recovered["key"],
                    "dependency_key": recovered["dependency_key"],
                })
        assert len({row["sha256"] for row in result["final_snapshots"]}) == 1
        assert len({row["key"] for row in result["final_snapshots"]}) == 1
        assert len({row["dependency_key"] for row in result["final_snapshots"]}) == 1
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = repr(error)
    result["checks"] = len(rows)
    result["processes"] = sum("exit_code" in row for row in rows)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result.get(key) for key in ("status", "checks", "processes", "error")}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
