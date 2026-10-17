"""Check workflow syntax and the foundation's required CI wiring without GitHub access."""

from pathlib import Path
import re
import sys

import yaml

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    path = ROOT / ".github/workflows/cpu.yml"
    try:
        # BaseLoader preserves GitHub's YAML `on` key instead of YAML 1.1 boolean conversion.
        workflow = yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        assert set(workflow["on"]) == {"push", "pull_request", "workflow_dispatch"}
        assert workflow["permissions"] == {"contents": "read"}
        jobs = workflow["jobs"]
        assert set(jobs) == {"quality", "cpu", "runtime-only"}
        rows = jobs["cpu"]["strategy"]["matrix"]["include"]
        assert len(rows) == 12
        assert len({row["id"] for row in rows}) == len(rows)
        assert {(row["os"], row["arch"]) for row in rows} == {
            ("windows-2025-vs2026", "x64"), ("ubuntu-24.04", "x64"),
            ("macos-26", "arm64"), ("macos-15-intel", "x64"),
        }
        for job in jobs.values():
            assert "timeout-minutes" in job
            for step in job["steps"]:
                assert "continue-on-error" not in step
                if "uses" in step:
                    assert re.fullmatch(r"actions/[a-z-]+@[a-f0-9]{40}", step["uses"])
                    if step["uses"].startswith("actions/checkout@"):
                        assert step["with"]["persist-credentials"] == "false"
                    if step["uses"].startswith("actions/upload-artifact@"):
                        assert step["if"] == "always()"
        dispatch = workflow["on"]["workflow_dispatch"]["inputs"]["inject_test_failure"]
        assert dispatch["type"] == "boolean" and dispatch["default"] == "false"
        assert "inputs.inject_test_failure" in jobs["cpu"]["env"]["SHARK_INJECT_FAILURE"]
        runners = [step for step in jobs["cpu"]["steps"] if "tools/ci.py" in step.get("run", "")]
        assert len(runners) == 2
        assert all("--inject-test-failure" in step["run"] for step in runners)
    except (AssertionError, KeyError, TypeError, OSError, yaml.YAMLError) as error:
        print(f"CI configuration check failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    print("CI YAML and required matrix/action/failure wiring passed; hosted execution NOT RUN.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
