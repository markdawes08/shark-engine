"""Check workflow syntax and the foundation's required CI wiring without GitHub access."""

from pathlib import Path
import hashlib
import json
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
        assert set(jobs) == {"quality", "cpu", "runtime-only", "headless-server"}
        rows = jobs["cpu"]["strategy"]["matrix"]["include"]
        assert len(rows) == 6
        assert len({row["id"] for row in rows}) == len(rows)
        assert {(row["os"], row["arch"]) for row in rows} == {
            ("windows-2025-vs2026", "x64"), ("ubuntu-24.04", "x64"),
        }
        expected_cpu_presets = {
            "windows-debug", "windows-release", "windows-asan",
            "linux-debug", "linux-release", "linux-asan-ubsan",
        }
        assert {row["preset"] for row in rows} == expected_cpu_presets
        assert all(row["id"] == row["preset"] for row in rows)
        presets = json.loads((ROOT / "CMakePresets.json").read_text())
        for kind in ("configurePresets", "buildPresets", "testPresets"):
            names = {preset["name"] for preset in presets[kind]}
            assert expected_cpu_presets <= names
            assert not any("macos" in name for name in names)
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
        headless = jobs["headless-server"]
        assert headless["runs-on"] == "ubuntu-24.04"
        assert "inputs.inject_test_failure" in headless["env"]["SHARK_INJECT_FAILURE"]
        commands = [step["run"] for step in headless["steps"] if "run" in step]
        assert len(commands) == 1 and "tools/ci/headless/run.sh" in commands[0]
        assert "--inject-test-failure" in commands[0]
        inputs = json.loads((ROOT / "tools/ci/headless/inputs.json").read_text())
        dockerfile = (ROOT / "tools/ci/headless/Dockerfile").read_text()
        setup = (ROOT / "tools/ci/headless/setup.sh").read_text()
        wrapper = (ROOT / "tools/ci/headless/run.sh").read_text()
        assert re.fullmatch(r"ubuntu@sha256:[0-9a-f]{64}", inputs["image"])
        assert f"FROM {inputs['image']}\n" in dockerfile
        assert inputs["bootstrap_ca_url"] in dockerfile
        assert f"--checksum=sha256:{inputs['bootstrap_ca_sha256']}" in dockerfile
        assert inputs["snapshot_uri"] in setup and "--no-install-recommends" in setup
        assert "--require-hashes" in setup and "--only-binary=:all:" in setup
        assert inputs["image"] in wrapper and "--network none --read-only" in wrapper
        assert "dst=/source,readonly" in wrapper and "mktemp -d" in wrapper
        assert "--run-for-ms 20" in wrapper
        assert (ROOT / "tools/ci/headless/packages.tsv").is_file()
        environment_lock = json.loads((ROOT / "dependencies.lock.json").read_text())["ci_environments"]["headless-ubuntu24-amd64"]
        assert environment_lock["image"] == inputs["image"]
        for file_key, hash_key in (("inputs_file", "inputs_sha256"),
                                   ("package_inventory", "package_inventory_sha256")):
            assert hashlib.sha256((ROOT / environment_lock[file_key]).read_bytes()).hexdigest() == environment_lock[hash_key]
    except (AssertionError, KeyError, TypeError, OSError, yaml.YAMLError) as error:
        print(f"CI configuration check failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    print("CI YAML and wiring for 9 Windows/Linux jobs passed; this check does not execute hosted CI.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
