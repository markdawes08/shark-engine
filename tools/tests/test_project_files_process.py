"""Exercise actual authoring owner exclusion and interrupted native publication."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    probe = args.probe.resolve()
    rows: list[dict] = []

    def run(label: str, mode: str, root: Path, *extra: str, expected: int = 0) -> None:
        command = [str(probe), mode, str(root), *extra]
        result = run_process(command, timeout=15)
        row = {"name": label, "command": command, "exit_code": result.returncode,
               "timed_out": result.timed_out, "cleanup_errors": result.cleanup_errors,
               "stdout": result.stdout, "stderr": result.stderr}
        row["status"] = "PASS" if (result.returncode == expected and not result.timed_out
                                           and not result.cleanup_errors) else "FAIL"
        if expected == 0 and row["status"] == "PASS":
            try:
                if json.loads(result.stdout.splitlines()[-1])["status"] != "PASS":
                    row["status"] = "FAIL"
            except (ValueError, KeyError, IndexError):
                row["status"] = "FAIL"
        rows.append(row)
        if row["status"] != "PASS":
            raise AssertionError(f"{label}: {row}")

    def check(label: str, condition: bool, **details) -> None:
        rows.append({"name": label, "status": "PASS" if condition else "FAIL", **details})
        if not condition:
            raise AssertionError(label)

    error = None
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        # CTest's writable build directory owns these fixtures, never the source mount.
        with tempfile.TemporaryDirectory(prefix="project-files-", dir=args.output.parent) as temporary:
            work = Path(temporary)
            baseline = work / "baseline"
            baseline.mkdir()
            run("baseline", "seed", baseline)
            before = (baseline / "scene.sharkscene").read_bytes()
            after_root = work / "expected-after"
            after_root.mkdir()
            run("expected-after", "seed-new", after_root)
            after = (after_root / "scene.sharkscene").read_bytes()
            recovered_root = work / "expected-recovered"
            recovered_root.mkdir()
            run("expected-recovered", "seed-recovered", recovered_root)
            recovered = (recovered_root / "scene.sharkscene").read_bytes()
            check("distinct-valid-complete-candidates", len({before, after, recovered}) == 3)

            for phase in ("staged", "written", "flushed", "before_publish", "published"):
                root = work / phase
                root.mkdir()
                run(f"{phase}.seed", "seed", root)
                run(f"{phase}.interrupt", "save-exit", root, phase, expected=73)
                actual = (root / "scene.sharkscene").read_bytes()
                expected = after if phase == "published" else before
                check(f"{phase}.whole-file", actual == expected,
                      sha256=hashlib.sha256(actual).hexdigest(), committed=phase == "published")
                stages = {path.name: path.read_bytes() for path in root.glob(".shark-stage-*")}
                check(f"{phase}.stage-count", len(stages) == (0 if phase == "published" else 1),
                      count=len(stages))
                run(f"{phase}.reopen", "inspect", root,
                    "After" if phase == "published" else "Before")
                run(f"{phase}.recovery", "recover", root)
                recovered_actual = (root / "scene.sharkscene").read_bytes()
                check(f"{phase}.recovery-whole-file", recovered_actual == recovered,
                      sha256=hashlib.sha256(recovered_actual).hexdigest())
                run(f"{phase}.recovery-reopen", "inspect", root, "Recovered")
                retained = {path.name: path.read_bytes() for path in root.glob(".shark-stage-*")}
                check(f"{phase}.foreign-stage-preserved", retained == stages,
                      artifacts={name: hashlib.sha256(data).hexdigest() for name, data in retained.items()})

            # Explicit pipe readiness/release bounds concurrency; no timing sleeps.
            holder = subprocess.Popen([str(probe), "hold", str(baseline)], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                      encoding="utf-8")
            try:
                with ThreadPoolExecutor(max_workers=1) as pool:
                    ready = pool.submit(holder.stdout.readline)
                    try:
                        line = ready.result(timeout=15)
                    except BaseException:
                        holder.kill()
                        holder.wait(timeout=5)
                        raise
                check("lock.ready", json.loads(line).get("state") == "ready")
                run("lock.second-process-rejected", "check-busy", baseline)
                stdout, stderr = holder.communicate("release\n", timeout=15)
                check("lock.normal-release", holder.returncode == 0, stdout=stdout, stderr=stderr)
            finally:
                if holder.poll() is None:
                    holder.kill()
                    holder.wait(timeout=5)
                for stream in (holder.stdin, holder.stdout, holder.stderr):
                    if stream is not None:
                        stream.close()
            run("lock.open-after-release", "inspect", baseline, "Before")
            run("lock.abrupt-owner-exit", "abort-lock", baseline, expected=73)
            run("lock.open-after-abrupt-exit", "inspect", baseline, "Before")
    except Exception as failure:
        error = str(failure)
    receipt = {"status": "FAIL" if error else "PASS", "rows": rows, "error": error,
               "scope": "local process exit and native file visibility; not power-loss recovery"}
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": receipt["status"], "checks": len(rows), "error": error}))
    return 1 if error else 0


if __name__ == "__main__":
    raise SystemExit(main())
