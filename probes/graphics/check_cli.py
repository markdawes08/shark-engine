"""Exercise only CPU-safe probe commands, including clean structured failures."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--lock", type=Path, required=True)
    args = parser.parse_args()
    digest = hashlib.sha256(args.lock.read_bytes()).hexdigest()
    sources = sorted(["probes/graphics/CMakeLists.txt", *(
        path.relative_to(args.lock.parent).as_posix()
        for path in (args.lock.parent / "probes/graphics").rglob("*")
        if path.is_file() and path.suffix in (".cpp", ".hpp", ".hlsl", ".manifest"))])
    material = "".join(
        f"{source}\n{hashlib.sha256((args.lock.parent / source).read_bytes()).hexdigest()}\n"
        for source in sources
    )
    source_digest = hashlib.sha256(material.encode("utf-8")).hexdigest()
    env = dict(os.environ)
    # Even on a graphics workstation these cases must not initialize a loader,
    # display or ICD. The paths intentionally name nonexistent providers.
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["VK_DRIVER_FILES"] = str(args.lock.parent / "does-not-exist-02a-icd.json")

    def run(arguments: list[str], expected: int, machine: bool = True) -> dict | str:
        result = subprocess.run(
            [str(args.executable), *arguments],
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=5,
            check=False,
            env=env,
        )
        assert result.returncode == expected, (arguments, result)
        if not machine:
            assert result.stdout.strip() and not result.stderr, result
            return result.stdout
        report = json.loads(result.stdout)
        assert report["schema_version"] == 2, report
        assert report["dependency_lock_sha256"] == digest, report
        assert report["probe_source_sha256"] == source_digest, report
        assert report["qualification"] == "NOT_RUN", report
        assert len(report["cases"]) == 8, report
        assert all(case["status"] == "NOT_RUN" for case in report["cases"]), report
        if expected == 0:
            assert not result.stderr and report["status"] == "PASS", result
        else:
            assert result.stderr and report["status"] == "FAIL", result
            assert report["error"]["code"] == "arguments", report
        return report

    assert "GraphicsProbe" in run(["--help"], 0, False)
    assert "GraphicsProbe" in run(["--version"], 0, False)
    description = run(["--describe"], 0)
    assert description["mode"] == "describe" and description["devices"] == [], description
    assert description["stages"] == [], description
    cases = [
        [],
        ["--smoke"],
        ["--cases"],
        ["--cases", "--device", "0" * 32],
        ["--smoke", "--device", "0" * 32, "--shader-dir", "."],
        ["--cases", "--device", "0" * 32, "--shader-dir", ".", "--artifacts-dir", ".", "--lifecycle-cycles", "101"],
        ["--cases", "--device", "0" * 32, "--shader-dir", ".", "--artifacts-dir", ".", "--device-cycles", "0"],
        ["--cases", "--device", "0" * 32, "--shader-dir", ".", "--artifacts-dir", ".", "--present-seconds", "601"],
        ["--cases", "--device", "0" * 32, "--shader-dir", ".", "--artifacts-dir", ".", "--inject-failure", "unknown"],
        ["--cases", "--device", "0" * 32, "--shader-dir", ".", "--artifacts-dir", ".", "--lifecycle-cycles", "-1"],
        ["--cases", "--device", "0" * 32, "--shader-dir", ".", "--artifacts-dir", ".", "--lifecycle-cycles", "1junk"],
        ["--device", "0" * 32],
        ["--inventory", "--device", "0" * 32],
        ["--describe", "--smoke"],
        ["--describe", "--describe"],
        ["--smoke", "--device", "0" * 32, "--device", "1" * 32],
        ["--smoke", "--device", "short"],
        ["--smoke", "--device", "G" * 32],
        ["--smoke", "--device", "0" * 32, "--validation", "maybe"],
        ["--inventory", "--validation", "off"],
        ["--smoke", "--device", "0" * 32, "--validation"],
        ["--help", "--surprise"],
    ]
    for arguments in cases:
        run(arguments, 2)
    hostile = '--unknown="quote"\\slash\n\r\t\x01'
    escaped = run([hostile], 2)
    assert hostile in escaped["error"]["message"], escaped
    # Exercise the actual native CRT argv boundary. Windows' legacy code page
    # loses these characters before main() unless the UTF-8 manifest is active.
    # Case path I/O and loader discovery are covered by the relocated GPU run.
    unicode_argument = "--unknown-path=C:\\Shark \u6e2c\u8a66 \U0001f9a9\\\u03bb"
    unicode_report = run([unicode_argument], 2)
    # The report writer escapes each non-ASCII source byte as U+00xx so even
    # arbitrary driver strings produce JSON. Recover those bytes for comparison.
    assert unicode_argument.encode("utf-8") in unicode_report["error"]["message"].encode("latin-1"), unicode_report
    print(f"PASS: {len(cases) + 5} CPU-only CLI/report cases; no GPU qualification")


if __name__ == "__main__":
    main()
