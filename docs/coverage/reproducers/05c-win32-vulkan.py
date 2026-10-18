"""Build/run the eight-lifetime Win32/Vulkan diagnostic on both current-PC GPUs.

Run from the repository root in its x64 MSVC environment. All output must be in a
fresh directory under out/. This is a diagnostic, never presentation qualification.
It performs no image acquisition, submission, presentation or foreign-handle cleanup.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import shutil
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from graphics_probe import (Unavailable, finite_float, layer_environment, reject_constant,
                            unique_object, validation_tools)
from process_runner import run_process

GPUS = {"nvidia": "863d6f59a51894ab8b4829aa9460c78d",
        "intel": "868088a7040000000002000000000000"}
LIFETIMES = 8


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def integer(value: object) -> bool:
    return type(value) is int and 0 <= value <= 0xFFFFFFFF


def inspect_trace(text: str, device: str) -> list[dict]:
    require(len(text.encode("utf-8")) <= 1024 * 1024, "Diagnostic output exceeded one MiB")
    rows = [json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant,
                       parse_float=finite_float) for line in text.splitlines()]
    require(len(rows) == 3 + 4 * LIFETIMES and all(isinstance(row, dict) for row in rows),
            "Incomplete or extra diagnostic rows")
    require(rows[0] == {"kind": "identity", "schema_version": 1, "qualification": "NOT_RUN",
                        "validation": "required", "selection": device, "lifetimes": LIFETIMES,
                        "acquired": 0, "submitted": 0, "presented": 0}, "Wrong diagnostic workload")
    require(all(type(rows[0][key]) is int for key in
                ("schema_version", "lifetimes", "acquired", "submitted", "presented")),
            "Identity counters must be integers")

    def sample(row: dict, cycle: int, stage: str) -> None:
        require(set(row) == {"kind", "cycle", "stage", "gdi", "user", "handles", "diagnostics", "cleanup_errors"}
                and row["kind"] == "sample" and type(row["cycle"]) is int and row["cycle"] == cycle
                and row["stage"] == stage, "Sample order or schema changed")
        require(all(integer(row[key]) for key in ("gdi", "user", "handles", "diagnostics", "cleanup_errors")),
                "Invalid resource/diagnostic counters")
        require(row["diagnostics"] == row["cleanup_errors"] == 0 and row["handles"] > 0,
                "Validation, cleanup or counter observation failed")

    sample(rows[1], 0, "initial")
    first_adapter = None
    for cycle in range(1, LIFETIMES + 1):
        offset = 2 + (cycle - 1) * 4
        adapter = rows[offset]
        require(set(adapter) == {"kind", "cycle", "device", "uuid", "driver_version", "driver_name", "driver_info"}
                and adapter["kind"] == "adapter" and type(adapter["cycle"]) is int
                and adapter["cycle"] == cycle and adapter["uuid"] == device
                and integer(adapter["driver_version"])
                and all(isinstance(adapter[key], str) and bool(adapter[key])
                        for key in ("device", "driver_name", "driver_info")), "Invalid adapter identity")
        current = {key: value for key, value in adapter.items() if key != "cycle"}
        if first_adapter is None:
            first_adapter = current
        require(current == first_adapter, "Adapter identity changed between lifetimes")
        for index, stage in enumerate(("before_swapchain", "after_swapchain", "closed"), start=1):
            sample(rows[offset + index], cycle, stage)
    sample(rows[-1], LIFETIMES, "final")
    return rows


def parser_mutations(rows: list[dict], device: str) -> list[str]:
    """Reject in-memory corruptions of the actual trace; never edit raw evidence."""
    def encoded(value: list[dict]) -> str:
        return "\n".join(json.dumps(row) for row in value) + "\n"

    candidates = {"missing_cycle": encoded(rows[:2] + rows[6:]),
                  "duplicated_cycle": encoded(rows[:6] + rows[2:6] + rows[10:]),
                  "malformed_line": encoded(rows) + "not-json\n"}
    for name, index, key, value in (("validation_error", 3, "diagnostics", 1),
                                    ("cleanup_error", 3, "cleanup_errors", 1),
                                    ("wrong_uuid", 2, "uuid", "0" * 32),
                                    ("invented_submission", 0, "submitted", 1),
                                    ("boolean_counter", 3, "gdi", True)):
        altered = [dict(row) for row in rows]
        altered[index][key] = value
        candidates[name] = encoded(altered)
    rejected = []
    for name, text in candidates.items():
        try:
            inspect_trace(text, device)
        except (ValueError, TypeError, KeyError):
            rejected.append(name)
        else:
            raise ValueError(f"Trace parser accepted the {name} mutation")
    return rejected


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="Fresh directory beneath repository out/")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/windows-debug",
                        help="Existing native build containing pinned Vulkan headers and loader")
    parser.add_argument("--validation-manifest", type=Path,
                        default=ROOT / "out/05a-work/validation-tools/summary.json")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("This diagnostic requires native Windows")
    output = args.output.resolve()
    if not output.is_relative_to((ROOT / "out").resolve()) or output == (ROOT / "out").resolve():
        parser.error("--output must be a new directory beneath repository out/")
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a fresh output directory; existing evidence is preserved: {error}")

    source = Path(__file__).with_suffix(".cpp")
    build = args.build_dir.resolve()
    validation_manifest = args.validation_manifest.resolve()
    manifest = ROOT / "engine/client/windows.manifest"
    executable = output / "RawVulkanGdi.exe"
    loader = build / "bin/vulkan-1.dll"
    loader_library = build / "_deps/vulkanloader-build/loader/vulkan-1.lib"
    headers = build / "_deps/vulkanheaders-src/include/vulkan"
    report = {"schema_version": 1, "checkpoint": "05c", "status": "FAIL", "qualification": "NOT_RUN",
              "scope": "Raw Win32/Vulkan only; eight fresh HWND/instance/device/surface/swapchain lifetimes per GPU; no acquisition/submission/presentation",
              "platform": platform.platform(), "started_utc": datetime.now(timezone.utc).isoformat(),
              "lifetimes_per_gpu": LIFETIMES, "native_deadline_seconds": 120, "phases": [], "runs": {}, "errors": []}

    def save() -> None:
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    def phase(name: str, command: list[str], timeout: float = 30, env=None):
        result = run_process(command, cwd=ROOT, timeout=timeout, env=env)
        (output / f"{name}.stdout.log").write_text(result.stdout, encoding="utf-8")
        (output / f"{name}.stderr.log").write_text(result.stderr, encoding="utf-8")
        report["phases"].append({"name": name, **asdict(result)})
        save()
        require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors,
                f"{name} failed; inspect retained stdout/stderr and process evidence")
        return result

    try:
        inputs = [source, Path(__file__).resolve(), manifest, ROOT / "dependencies.lock.json",
                  loader, loader_library, build / "CMakeCache.txt", validation_manifest,
                  *[headers / name for name in ("vulkan.h", "vulkan_core.h", "vulkan_win32.h", "vk_platform.h")],
                  *[ROOT / "tools" / name for name in ("process_runner.py", "graphics_probe.py", "windows_unelevated.py", "toolchain_report.py")]]
        report["input_sha256"] = {str(path): sha256(path) for path in inputs}
        report["source_sha256"] = sha256(source)
        report["lock_sha256"] = sha256(ROOT / "dependencies.lock.json")
        validation = validation_tools(validation_manifest, report["lock_sha256"])
        report["validation_tools"] = {"manifest": str(validation_manifest), "manifest_sha256": sha256(validation_manifest), "receipt": validation}
        compiler = shutil.which("cl")
        require(compiler is not None, "Use the repository x64 MSVC developer wrapper")
        report["compiler"] = {"path": compiler, "sha256": sha256(Path(compiler))}
        command = [compiler, "/nologo", "/std:c++20", "/EHsc", "/MDd", "/W4", "/WX", "/utf-8",
                   "/I" + str(headers.parent), str(source), "/Fo" + str(output / "raw.obj"),
                   "/Fe" + str(executable), "/link", "/MACHINE:X64", "/MANIFEST:EMBED",
                   "/MANIFESTINPUT:" + str(manifest), str(loader_library), "user32.lib", "gdi32.lib"]
        report["independence"] = {"proof": "Reviewed standalone source and exact compile/link command; no Engine, GLFW or NVRHI source/library inputs", "compile_link_command": command}
        phase("compile", command, 120)
        shutil.copy2(loader, output / "vulkan-1.dll")
        report["executable_sha256"] = sha256(executable)
        report["loader_sha256"] = sha256(output / "vulkan-1.dll")
        imports = phase("imports", ["dumpbin", "/imports", str(executable)])
        require("vulkan-1.dll" in imports.stdout.lower(), "Pinned Vulkan loader import absent")
        report["independence"]["dynamic_import_listing"] = str(output / "imports.stdout.log")
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Required core/synchronization validation; no overrides.\n", encoding="utf-8")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, report["layer_environment"] = layer_environment(validation, settings, implicit)
        require(not report["layer_environment"]["driver_selection"], "Remove ambient Vulkan driver overrides before running")
        for label, device in GPUS.items():
            try:
                token_path = output / f"{label}-token.json"
                result = phase(label, [sys.executable, str(ROOT / "tools/windows_unelevated.py"),
                                      "--timeout", "120", "--evidence", str(token_path), "--", str(executable), device], 150, environment)
                require(not result.stderr, f"{label} emitted unexpected diagnostics")
                token = json.loads(token_path.read_text(encoding="utf-8"))
                security = token["security"]
                require(token["status"] == "PASS" and security["same_user"] and security["same_session"]
                        and security["child"]["integrity_level"] == "medium" and not security["child"]["elevated"],
                        "Native diagnostic did not use the same-user standard desktop token")
                require(token["result"]["stdout"] == result.stdout and token["result"]["stderr"] == result.stderr
                        and token["result"]["returncode"] == 0 and not token["result"]["timed_out"]
                        and not token["result"]["cleanup_errors"], "Token and process trace disagree")
                rows = inspect_trace(result.stdout, device)
                closed = [row for row in rows if row.get("stage") == "closed"]
                report["runs"][label] = {"device": device, "rows": rows, "token_path": str(token_path),
                                         "token_sha256": sha256(token_path), "parser_mutations_rejected": parser_mutations(rows, device),
                                         "closed_gdi": [row["gdi"] for row in closed],
                                         "closed_user": [row["user"] for row in closed], "closed_kernel": [row["handles"] for row in closed]}
            except (OSError, ValueError, KeyError, TypeError) as error:
                report["errors"].append(f"{label}: {error}")
            save()
        require(all(sha256(Path(path)) == digest for path, digest in report["input_sha256"].items()), "Diagnostic inputs changed during build/run")
        require(sha256(executable) == report["executable_sha256"] and sha256(output / "vulkan-1.dll") == report["loader_sha256"]
                and sha256(Path(compiler)) == report["compiler"]["sha256"], "Executable/loader/compiler identity changed")
        require(validation_tools(validation_manifest, report["lock_sha256"]) == validation, "Pinned validation tools changed")
        report["identities_unchanged"] = True
        require(set(report["runs"]) == set(GPUS) and not report["errors"], "Both strict diagnostic traces are required")
        report["status"] = "DIAGNOSTIC_ONLY"
    except Unavailable as error:
        report["errors"].append(str(error))
        report["status"] = "NOT_RUN"
    except (OSError, ValueError, KeyError, TypeError) as error:
        report["errors"].append(str(error))
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps({"status": report["status"], "qualification": report["qualification"], "summary": str(output / "summary.json")}))
    return 0 if report["status"] == "DIAGNOSTIC_ONLY" else 1


if __name__ == "__main__":
    raise SystemExit(main())
