"""Actual Windows PowerShell package-launcher CPU contract and process tests.

Uses a tiny synthetic executable under an owned output directory. These tests
exercise the launcher, never claim graphics execution, and require no GPU/SDK.
Windows PowerShell's built-in Add-Type compiles the fixture only on this
development host; the generated qualification package does not contain it.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process
from graphics_probe import REQUIRED_ARTIFACTS, REQUIRED_CHECKS

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "tools/graphics_package/Run-GraphicsProbe.ps1"
ZERO = "0" * 64
FIXTURE = r'''
using System;
using System.IO;
using System.Diagnostics;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
class ProbeFixture {
    static int Main(string[] args) {
        Console.OutputEncoding=new UTF8Encoding(false);
        if(args.Length>0 && args[0]=="--fixture-child") { Thread.Sleep(30000); return 0; }
        string root=AppDomain.CurrentDomain.BaseDirectory;
        string scenario=File.ReadAllText(Path.Combine(root,"scenario.txt"));
        if(scenario=="timeout") {
            ProcessStartInfo info=new ProcessStartInfo(Process.GetCurrentProcess().MainModule.FileName,"--fixture-child");
            info.UseShellExecute=false; info.CreateNoWindow=true;
            Process child=Process.Start(info);
            File.WriteAllText(Environment.GetEnvironmentVariable("SHARK_FIXTURE_PID"),child.Id.ToString());
            Thread.Sleep(30000); return 0;
        }
        if(scenario=="flood") { for(int n=0;n<2048;n++) Console.Write(new string('x',8192)); return 0; }
        if(scenario=="echo") {
            Console.Write(new JavaScriptSerializer().Serialize(new { argv=args, controlled=Environment.GetEnvironmentVariable("VK_LAYER_FIXTURE") })); return 0;
        }
        if(scenario=="mutate") File.AppendAllText(Path.Combine(root,"scenario.txt"),"changed");
        if(scenario=="cases") {
            int index=Array.IndexOf(args,"--artifacts-dir");
            if(index<0 || index+1>=args.Length) return 2;
            Directory.CreateDirectory(args[index+1]);
            foreach(string file in Directory.GetFiles(Path.Combine(root,"readbacks"))) File.Copy(file,Path.Combine(args[index+1],Path.GetFileName(file)));
        }
        Console.Write(File.ReadAllText(Path.Combine(root,"report.json")));
        return scenario=="exit3" ? 3 : 0;
    }
}
'''


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ps_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def encoded_command(text: str) -> list[str]:
    encoded = base64.b64encode(text.encode("utf-16-le")).decode("ascii")
    return ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded]


def base_report() -> dict:
    return {"schema_version": 2, "mode": "describe", "status": "PASS", "qualification": "NOT_RUN",
            "dependency_lock_sha256": ZERO, "probe_source_sha256": ZERO, "exit_code": 0,
            "cases": [{"id": f"P02-{i:02}", "status": "NOT_RUN"} for i in range(1, 9)],
            "stages": [], "devices": [], "selected_device_uuid": None,
            "vulkan_validation_enabled": False, "nvrhi_validation_enabled": False,
            "validation_warnings": 0, "validation_errors": 0}


def manifest(package: Path) -> None:
    value = {"schema_version": 1, "status": "PASS", "qualification": "NOT_RUN",
             "probe": {"schema_version": 2, "dependency_lock_sha256": ZERO,
                       "probe_source_sha256": ZERO, "executable": "bin/GraphicsProbe.exe"},
             "files": {path.relative_to(package).as_posix(): {"sha256": digest(path), "bytes": path.stat().st_size}
                       for path in sorted(package.rglob("*")) if path.is_file() and path.name != "package-manifest.json"}}
    (package / "package-manifest.json").write_text(json.dumps(value), encoding="utf-8")


def package_fixture(root: Path, executable: Path, scenario: str = "normal", report=None) -> Path:
    package = root / "package space é"
    (package / "bin").mkdir(parents=True)
    (package / "shaders").mkdir()
    (package / "validation/implicit").mkdir(parents=True)
    shutil.copy2(executable, package / "bin/GraphicsProbe.exe")
    shutil.copy2(LAUNCHER, package / "Run-GraphicsProbe.ps1")
    (package / "bin/scenario.txt").write_text(scenario, encoding="utf-8")
    (package / "bin/report.json").write_text(json.dumps(base_report() if report is None else report), encoding="utf-8")
    if scenario == "cases":
        readbacks = package / "bin/readbacks"
        readbacks.mkdir()
        for artifacts in REQUIRED_ARTIFACTS.values():
            for name, (kind, width, height, layers) in artifacts.items():
                pixel_bytes = 8 if kind == "rgba16f-le" else 4
                (readbacks / name).write_bytes(bytes(width * height * layers * pixel_bytes))
    for name in ("fixture_vs", "fixture_ps", "compute_cs", "sample_ps"):
        (package / f"shaders/{name}.spv").write_bytes(b"synthetic-not-a-shader")
    (package / "bin/vulkan-1.dll").write_bytes(b"synthetic-not-a-library")
    (package / "validation/VkLayer_khronos_validation.dll").write_bytes(b"synthetic-not-a-library")
    (package / "validation/vk_layer_settings.txt").write_bytes(b"")
    (package / "validation/VkLayer_khronos_validation.json").write_text(json.dumps({"layer": {
        "name": "VK_LAYER_KHRONOS_validation", "library_path": ".\\VkLayer_khronos_validation.dll"}}), encoding="utf-8")
    manifest(package)
    return package


def invoke(package: Path, output: Path, *args: str, env: dict | None = None, standard=False):
    command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
               "-File", str(package / "Run-GraphicsProbe.ps1"), "-Output", str(output), *args]
    if standard:
        command = [sys.executable, str(ROOT / "tools/windows_unelevated.py"), "--timeout", "30", "--", *command]
    result = run_process(command, cwd=ROOT, timeout=45, env=env)
    if result.timed_out or result.cleanup_errors:
        raise AssertionError(f"Outer harness cleanup/deadline failure: {result}")
    value = json.loads(result.stdout)
    if value["exit_code"] != result.returncode:
        raise AssertionError(f"Launcher process/receipt code mismatch: {result.returncode}, {value}")
    return result, value


def process_exists(pid: int) -> bool:
    import ctypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_uint, ctypes.c_bool, ctypes.c_uint]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return False
    code = ctypes.c_uint()
    try:
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return code.value == 259
    finally:
        kernel.CloseHandle(handle)


def cases_report() -> dict:
    value = base_report()
    value.update(mode="cases", vulkan_validation_enabled=True, nvrhi_validation_enabled=True,
                 selected_device_uuid="0" * 32, devices=[{"uuid": "0" * 32, "reported_profile_supported": True}],
                 swapchain={"image_count": 2, "extent": [64, 64], "present_mode": "FIFO", "presented_frames": 5},
                 validation_configuration={"core": True, "synchronization": True, "gpu_assisted": False, "disabled_checks": []},
                 lifecycle={"requested_resize_cycles": 2, "completed_resize_cycles": 2, "requested_device_cycles": 1,
                            "completed_device_cycles": 1, "minimize_restore_cycles": 2, "observed_zero_framebuffer_extents": 2,
                            "zero_extent_policy_checks": 0, "presented_frames": 5, "requested_present_seconds": 0,
                            "present_elapsed_seconds": 0, "fully_qualified": False})
    value["stages"] = [{"name": name, "status": "PASS"} for name in ("loader", "instance", "surface", "inventory", "device",
                       "nvrhi_device_and_validation", "swapchain", "readback_cases", "lifecycle", "teardown")]
    for case in value["cases"]:
        identifier = case["id"]
        if identifier in ("P02-01", "P02-02", "P02-03", "P02-04", "P02-06"):
            case["status"] = "PASS"
        if identifier in REQUIRED_CHECKS:
            case["execution_status"] = "PASS"
            case["checks"] = [{"name": name, "status": "PASS", "compared_values": minimum, "failed_values": 0,
                               "absolute_tolerance": absolute, "relative_tolerance": relative,
                               "max_absolute_error": 0, "max_tolerance_ratio": 0}
                              for name, (minimum, absolute, relative) in REQUIRED_CHECKS[identifier].items()]
            case["artifacts"] = [{"path": name, "format": kind, "width": width, "height": height, "layers": layers,
                                  "bytes": width * height * layers * (8 if kind == "rgba16f-le" else 4)}
                                 for name, (kind, width, height, layers) in REQUIRED_ARTIFACTS[identifier].items()]
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt":
        print("NOT_RUN: native Windows PowerShell integration is required")
        return 3
    output = args.output.resolve()
    if output.exists():
        parser.error("Use a fresh output directory")
    output.mkdir(parents=True)
    fixture = output / "ProbeFixture.exe"
    source = output / "fixture.cs"
    source.write_text(FIXTURE, encoding="utf-8")
    compile_code = ("$ErrorActionPreference='Stop'; Add-Type -TypeDefinition ([IO.File]::ReadAllText(" +
                    ps_literal(str(source)) + ")) -ReferencedAssemblies System.Web.Extensions -OutputAssembly " +
                    ps_literal(str(fixture)) + " -OutputType ConsoleApplication")
    build = run_process(encoded_command(compile_code), cwd=ROOT, timeout=30)
    if build.returncode:
        raise RuntimeError(build.stderr + build.stdout)
    results = []

    def record(name: str, function):
        started = time.monotonic()
        try:
            detail = function()
            results.append({"name": name, "status": "PASS", "elapsed_seconds": time.monotonic() - started,
                            "detail": detail})
        except Exception as error:
            results.append({"name": name, "status": "FAIL", "error": str(error)})
        print(name + ": " + results[-1]["status"], flush=True)

    def basic(name, expected, *, scenario="normal", report=None, arguments=("-Mode", "describe"), change=None, standard=False):
        root = output / name
        package = package_fixture(root, fixture, scenario, report)
        if change:
            change(package)
        result, value = invoke(package, root / "evidence space é", *arguments, standard=standard)
        assert result.returncode == expected, value
        assert value["qualification"] == "NOT_RUN", value
        return {"exit_code": result.returncode, "errors": value["errors"], "process": value["process"]}

    record("description", lambda: basic("description", 0))
    record("invalid_mode", lambda: basic("invalid_mode", 2, arguments=("-Mode", "bogus")))
    record("invalid_uuid", lambda: basic("invalid_uuid", 2, arguments=("-Mode", "cases", "-Device", "bad")))
    record("invalid_deadline", lambda: basic("invalid_deadline", 2, arguments=("-Mode", "describe", "-TimeoutSeconds", "0")))

    def fresh_output():
        root = output / "fresh_output"
        package = package_fixture(root, fixture)
        existing = root / "existing"
        existing.mkdir()
        (existing / "sentinel").write_bytes(b"unchanged")
        result, value = invoke(package, existing, "-Mode", "describe")
        assert result.returncode == 2 and sorted(p.name for p in existing.iterdir()) == ["sentinel"], value
        return value["errors"]
    record("fresh_output", fresh_output)

    def output_junction():
        root = output / "output_junction"
        package = package_fixture(root, fixture)
        link = root / "junction"
        code = "New-Item -ItemType Junction -Path " + ps_literal(str(link)) + " -Target " + ps_literal(str(package)) + " | Out-Null"
        linked = run_process(encoded_command(code), cwd=ROOT, timeout=10)
        assert linked.returncode == 0, linked.stderr
        try:
            result, value = invoke(package, link / "must-not-exist", "-Mode", "describe")
            assert result.returncode == 2 and not (package / "must-not-exist").exists(), value
            return value["errors"]
        finally:
            # Removes only the junction entry, never recursively follows it.
            link.rmdir()
    record("output_junction_rejected", output_junction)
    record("changed_package", lambda: basic("changed_package", 1, change=lambda path: (path / "bin/vulkan-1.dll").write_bytes(b"changed")))
    record("unlisted_file", lambda: basic("unlisted_file", 1, change=lambda path: (path / "bin/unexpected.dll").write_bytes(b"extra")))
    def forward_slash(path):
        layer_path = path / "validation/VkLayer_khronos_validation.json"
        layer = json.loads(layer_path.read_text())
        layer["layer"]["library_path"] = "./VkLayer_khronos_validation.dll"
        layer_path.write_text(json.dumps(layer), encoding="utf-8")
        manifest(path)
    record("windows_layer_path_required", lambda: basic("windows_layer_path_required", 1, change=forward_slash))
    bad = base_report()
    bad["probe_source_sha256"] = "1" * 64
    record("stale_binary_report", lambda: basic("stale_binary_report", 1, report=bad))
    bad = base_report()
    bad["cases"] = bad["cases"][:-1]
    record("missing_case", lambda: basic("missing_case", 1, report=bad))
    bad = base_report()
    bad["schema_version"] = "2"
    record("string_schema_rejected", lambda: basic("string_schema_rejected", 1, report=bad))
    bad = base_report()
    bad.update(mode="inventory", devices=42,
               stages=[{"name": name, "status": "PASS"} for name in ("loader", "instance", "surface", "inventory", "teardown")])
    record("scalar_inventory_rejected", lambda: basic("scalar_inventory_rejected", 1, report=bad, arguments=("-Mode", "inventory")))

    def duplicate(path):
        report = json.dumps(base_report())[:-1] + ',"status":"PASS"}'
        (path / "bin/report.json").write_text(report, encoding="utf-8")
        manifest(path)
    record("duplicate_json", lambda: basic("duplicate_json", 1, change=duplicate))
    unavailable = base_report()
    unavailable.update(status="NOT_RUN", exit_code=3, error={"code": "fixture_unavailable", "message": "Synthetic CPU unavailable fixture"})
    record("native_unavailable", lambda: basic("native_unavailable", 3, scenario="exit3", report=unavailable))
    record("post_run_mutation", lambda: basic("post_run_mutation", 1, scenario="mutate"))
    record("bounded_output", lambda: basic("bounded_output", 1, scenario="flood"))

    def deadline():
        root = output / "deadline"
        package = package_fixture(root, fixture, "timeout")
        child = root / "child.pid"
        env = dict(os.environ, SHARK_FIXTURE_PID=str(child))
        result, value = invoke(package, root / "evidence", "-Mode", "describe", "-TimeoutSeconds", "1", env=env)
        assert result.returncode == 124 and value["process"]["cleanup_complete"], value
        assert child.exists(), "Timeout fixture did not spawn its child"
        pid = int(child.read_text())
        assert not process_exists(pid), f"Owned descendant {pid} survived timeout"
        return {"process": value["process"], "descendant_pid": pid, "descendant_absent": True}
    record("timeout_descendant_cleanup", deadline)

    def abrupt():
        root = output / "abrupt"
        package = package_fixture(root, fixture, "timeout")
        child = root / "child.pid"
        env = dict(os.environ, SHARK_FIXTURE_PID=str(child))
        command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                   str(package / "Run-GraphicsProbe.ps1"), "-Output", str(root / "evidence"), "-Mode", "describe"]
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 15
            while not child.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            assert child.exists(), "Fixture child not observed before bounded readiness deadline"
            pid = int(child.read_text())
            process.kill()
            process.communicate(timeout=5)
            deadline = time.monotonic() + 5
            while process_exists(pid) and time.monotonic() < deadline:
                time.sleep(0.02)
            assert not process_exists(pid), f"Owned descendant {pid} survived abrupt launcher termination"
            return {"descendant_pid": pid, "descendant_absent": True}
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
    record("abrupt_launcher_descendant_cleanup", abrupt)

    # Invoke the exact embedded process helper directly to test arbitrary argv
    # that the fixed package command normally never needs to supply.
    def quoting():
        root = output / "quoting"
        package = package_fixture(root, fixture, "echo")
        expected = ["", "two words", 'embedded"quote', "trailing\\", 'slash\\\"quote', "é λ"]
        content = LAUNCHER.read_text(encoding="utf-8")
        native = content.split("Add-Type -TypeDefinition @'\n", 1)[1].split("\n'@", 1)[0]
        native_path = root / "native.cs"
        native_path.write_text(native, encoding="utf-8")
        code = ("$ErrorActionPreference='Stop'; Add-Type -TypeDefinition ([IO.File]::ReadAllText(" + ps_literal(str(native_path)) + ")); "
                "$envmap=New-Object 'System.Collections.Generic.Dictionary[string,string]'; "
                "foreach($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {$envmap[$entry.Key]=$entry.Value}; "
                "$result=[SharkPackageNative]::Run(" + ps_literal(str(package / "bin/GraphicsProbe.exe")) + ", [string[]]@(" +
                ",".join(ps_literal(arg) for arg in expected) + ")," + ps_literal(str(root)) + ",$envmap,5,8388608); "
                "$result | ConvertTo-Json -Depth 8")
        result = run_process(encoded_command(code), cwd=ROOT, timeout=30)
        assert result.returncode == 0, result.stderr
        value = json.loads(result.stdout)
        assert value["exit_code"] == 0 and value["cleanup_complete"], value
        assert json.loads(value["stdout"])["argv"] == expected, value
        return {"exact_arguments": expected, "cleanup_complete": True}
    record("exact_windows_argv", quoting)
    case_arguments = ("-Mode", "cases", "-Device", "0" * 32)
    record("synthetic_cases_contract", lambda: basic("synthetic_cases_contract", 0, scenario="cases", report=cases_report(),
           arguments=case_arguments, standard=True))
    bad = cases_report()
    bad["validation_configuration"]["core"] = "True"
    record("string_validation_rejected", lambda: basic("string_validation_rejected", 1, scenario="cases", report=bad,
           arguments=case_arguments, standard=True))
    bad = cases_report()
    bad["devices"][0]["reported_profile_supported"] = "True"
    record("string_capability_rejected", lambda: basic("string_capability_rejected", 1, scenario="cases", report=bad,
           arguments=case_arguments, standard=True))
    bad = cases_report()
    bad["cases"][1]["checks"][0]["absolute_tolerance"] = 0.5
    record("widened_tolerance_rejected", lambda: basic("widened_tolerance_rejected", 1, scenario="cases", report=bad,
           arguments=case_arguments, standard=True))
    status = "PASS" if all(item["status"] == "PASS" for item in results) else "FAIL"
    receipt = {"schema_version": 1, "status": status, "qualification": "NOT_RUN",
               "scope": "Actual Windows CPU launcher integration using synthetic executables; no GPU claims",
               "launcher_sha256": digest(LAUNCHER), "fixture_sha256": digest(fixture), "checks": results}
    (output / "summary.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
