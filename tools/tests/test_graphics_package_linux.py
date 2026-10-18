"""CPU fixtures for Linux package rejection, relocation and process containment.

Synthetic executables below never establish native GPU or clean-machine coverage.
"""

from __future__ import annotations

import importlib.util
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import package_graphics_probe_linux as package
from process_runner import ProcessResult

spec = importlib.util.spec_from_file_location("linux_package_launcher", TOOLS / "graphics_package/run_graphics_probe.py")
launcher = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = launcher
spec.loader.exec_module(launcher)


def elf_fixture() -> bytes:
    result = bytearray(64)
    result[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", result, 18, 62)
    return bytes(result)


def elf_text(imports=("libc.so.6",), versions=("GLIBC_2.39",)) -> str:
    return ("".join(f" 0 (NEEDED) Shared library: [{name}]\n" for name in imports)
            + "".join(f" 0 Name: {name}\n" for name in versions))


def retire_fixture(pid: int, command_marker: str) -> None:
    """Test-failure cleanup must still own the recorded PID and process group."""
    try:
        command = Path(f"/proc/{pid}/cmdline").read_bytes()
        if command_marker.encode() not in command:
            return  # Already gone/zombie or PID identity changed.
        group = os.getpgid(pid)
        if group == os.getpgrp() or group <= 1:
            raise AssertionError("Fixture unexpectedly shares the test controller process group")
        os.killpg(group, signal.SIGKILL)
    except (ProcessLookupError, FileNotFoundError):
        pass


def fixture_payload() -> tuple[dict, dict]:
    lock = b'{"test_fixture":true}\n'
    lock_hash = package.digest(lock)
    source_hash = "a" * 64
    native = {"schema_version": 2, "mode": "describe", "dependency_lock_sha256": lock_hash,
              "probe_source_sha256": source_hash, "qualification": "NOT_RUN", "status": "PASS", "exit_code": 0,
              "cases": [{"id": f"P02-{n:02d}", "status": "NOT_RUN"} for n in range(1, 9)],
              "stages": [], "devices": [], "selected_device_uuid": None, "vulkan_validation_enabled": False,
              "nvrhi_validation_enabled": False, "validation_warnings": 0, "validation_errors": 0}
    payload = {"bin/GraphicsProbe": ("#!/usr/bin/python3\nprint(" + repr(json.dumps(native)) + ")\n").encode(),
               "lib/libvulkan.so.1": b"CPU fixture", "validation/libVkLayer_khronos_validation.so": b"CPU fixture",
               "validation/VkLayer_khronos_validation.json": package.json_bytes({"layer": {
                   "name": "VK_LAYER_KHRONOS_validation", "library_path": "./libVkLayer_khronos_validation.so", "api_version": "1.4.352"}}),
               "validation/vk_layer_settings.txt": b"", "licenses/fixture/LICENSE.txt": b"Synthetic CPU fixture only",
               "README.txt": b"CPU synthetic fixture; no GPU qualification", "provenance/dependencies.lock.json": lock,
               **{f"provenance/{name}.json": b"{}" for name in ("source", "toolchain", "shader-manifest", "validation-manifest", "dependency-closure", "probe-description")},
               **{"shaders/" + name: b"CPU fixture" for name in launcher.SHADERS},
               "run_graphics_probe.py": (TOOLS / "graphics_package/run_graphics_probe.py").read_bytes(),
               **{"_support/" + name: (TOOLS / name).read_bytes() for name in ("graphics_probe.py", "process_runner.py", "toolchain_report.py")}}
    manifest = {"schema_version": 1, "assignment": "02d", "platform": "linux-x86_64", "configuration": "Release",
                "status": "PASS", "qualification": "NOT_RUN", "dependency_lock_sha256": lock_hash,
                "probe_source_sha256": source_hash,
                "probe": {"schema_version": 2, "executable": "bin/GraphicsProbe", "dependency_lock_sha256": lock_hash, "probe_source_sha256": source_hash},
                "validation": {"manifest": "validation/VkLayer_khronos_validation.json",
                               "library": "validation/libVkLayer_khronos_validation.so", "api_version": "1.4.352"}}
    return payload, manifest


class LinuxPackageTests(unittest.TestCase):
    def test_elf_machine_and_endianness_required(self):
        package.elf_x64(elf_fixture(), "fixture")
        wrong = bytearray(elf_fixture())
        struct.pack_into("<H", wrong, 18, 183)
        for data in (b"not ELF", bytes(wrong), b"\x7fELF\x01\x01" + b"\0" * 58):
            with self.subTest(data=data[:6]), self.assertRaisesRegex(ValueError, "x86-64"):
                package.elf_x64(data, "fixture")

    def test_required_symbols_and_runtime_paths_enforce_baseline(self):
        result = package.parse_elf(elf_text(("libc.so.6", "libvulkan.so.1")))
        self.assertEqual(result["imports"], ["libc.so.6", "libvulkan.so.1"])
        bad = ["", elf_text(("../libescape.so",)), elf_text(versions=("GLIBC_2.40",)),
               elf_text(versions=("GLIBCXX_3.4.34",)), elf_text(versions=("CXXABI_1.3.16",)),
               elf_text(versions=("GLIBC_PRIVATE",)), elf_text() + " 0 (RUNPATH) Library runpath: [/source/build]\n",
               elf_text() + " [Requesting program interpreter: /bad/ld.so]\n"]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ValueError):
                package.parse_elf(text)

    def test_recursive_packaged_closure_includes_layer_and_rejects_external_compiler(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inspector = root / "readelf"
            inspector.write_bytes(b"synthetic inspector")
            binaries = {}
            for name in ("bin/GraphicsProbe", "lib/libvulkan.so.1", "validation/layer.so"):
                path = root / name
                path.parent.mkdir(parents=True)
                path.write_bytes(elf_fixture())
                binaries[name] = path
            with patch.object(package, "run_process", return_value=ProcessResult([], 0, elf_text(), "", False)) as run:
                self.assertEqual(len(package.inspect_closure(binaries, inspector)["binaries"]), 3)
                self.assertEqual(run.call_count, 3)
            with patch.object(package, "run_process", return_value=ProcessResult([], 0, elf_text(("libdxcompiler.so",)), "", False)):
                with self.assertRaisesRegex(ValueError, "Unpackaged"):
                    package.inspect_closure(binaries, inspector)

    def test_inspector_timeout_or_failure_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary, inspector = root / "probe", root / "readelf"
            binary.write_bytes(elf_fixture())
            inspector.write_bytes(b"inspector")
            for result in (ProcessResult([], 1, "", "", False), ProcessResult([], 0, elf_text(), "", True),
                           ProcessResult([], 0, elf_text(), "", False, ("cleanup failed",))):
                with patch.object(package, "run_process", return_value=result), self.assertRaisesRegex(ValueError, "inspection failed"):
                    package.inspect_closure({"bin/GraphicsProbe": binary}, inspector)

    def test_release_and_source_cache_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            values = {"CMAKE_BUILD_TYPE": "Release", "SHARK_SANITIZER": "none", "SHARK_BUILD_GRAPHICS_PROBE": "ON",
                      "CMAKE_HOME_DIRECTORY": str(package.ROOT)}
            cache = root / "CMakeCache.txt"
            def write():
                cache.write_text("".join(f"{k}:STRING={v}\n" for k, v in values.items()), encoding="utf-8")
            write()
            package.cache_values(root, root / "bin/GraphicsProbe")
            for key, value in (("CMAKE_BUILD_TYPE", "Debug"), ("SHARK_SANITIZER", "address"),
                               ("SHARK_BUILD_GRAPHICS_PROBE", "OFF"), ("CMAKE_HOME_DIRECTORY", str(root))):
                old = values[key]
                values[key] = value
                write()
                with self.assertRaises(ValueError):
                    package.cache_values(root, root / "bin/GraphicsProbe")
                values[key] = old

    def test_publication_and_verified_payload_are_fresh(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            payload, manifest = fixture_payload()
            expected = package.publish(output, payload, manifest)
            self.assertEqual(launcher.verify_package(output), expected)
            with self.assertRaisesRegex(ValueError, "fresh"):
                package.publish(output, payload, manifest)
            if os.name == "posix":
                self.assertTrue(os.access(output / "bin/GraphicsProbe", os.X_OK))

    def test_payload_tampering_missing_or_extra_files_are_rejected(self):
        for change in ("hash", "missing", "extra"):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "package"
                package.publish(output, *fixture_payload())
                target = output / "shaders/fixture_vs.spv"
                if change == "hash":
                    target.write_bytes(b"corrupt")
                elif change == "missing":
                    target.unlink()
                else:
                    (output / "unexpected.py").write_bytes(b"unexpected")
                with self.assertRaises(ValueError):
                    launcher.verify_package(output)

    def test_manifest_boolean_schema_counts_and_escaping_paths_rejected(self):
        for change in ("schema", "bytes", "path", "probe_identity", "layer"):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "package"
                manifest = package.publish(output, *fixture_payload())
                if change == "schema":
                    manifest["schema_version"] = True
                elif change == "bytes":
                    manifest["files"]["README.txt"]["bytes"] = True
                elif change == "path":
                    manifest["files"]["../outside"] = manifest["files"]["README.txt"]
                elif change == "probe_identity":
                    manifest["probe"]["dependency_lock_sha256"] = "b" * 64
                else:
                    manifest["validation"]["manifest"] = "../external.json"
                (output / "package-manifest.json").write_bytes(package.json_bytes(manifest))
                with self.assertRaises(ValueError):
                    launcher.verify_package(output)

    def test_strict_json_rejects_duplicate_keys_nonfinite_overflow(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "manifest.json"
            for text in ('{"x":1,"x":2}', '{"x":NaN}', '{"x":1e999}', '[]'):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    launcher.load_json(path)

    def test_child_environment_cannot_inherit_driver_or_loader_overrides(self):
        with patch.dict(os.environ, {"LD_PRELOAD": "/untrusted.so", "LD_LIBRARY_PATH": "/sdk", "VK_DRIVER_FILES": "/wrong.json",
                                     "VK_LAYER_PATH": "/sdk", "PYTHONPATH": "/external", "DISPLAY": ":0"}, clear=True):
            env = launcher.child_environment(Path("/fixture"), Path("/evidence"))
        self.assertNotIn("LD_PRELOAD", env)
        self.assertNotIn("VK_DRIVER_FILES", env)
        self.assertNotIn("PYTHONPATH", env)
        self.assertEqual(env["LD_LIBRARY_PATH"], str(Path("/fixture/lib")))
        self.assertEqual(env["DISPLAY"], ":0")

    @unittest.skipUnless(sys.platform == "linux", "Actual Linux process/permission fixture")
    def test_symlinked_package_payload_and_output_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "package"
            package.publish(output, *fixture_payload())
            target = output / "README.txt"
            target.unlink()
            target.symlink_to(root / "outside")
            with self.assertRaisesRegex(ValueError, "Symbolic"):
                launcher.verify_package(output)
            fresh = root / "fresh-package"
            package.publish(fresh, *fixture_payload())
            (root / "real-parent").mkdir()
            (root / "linked-parent").symlink_to(root / "real-parent", target_is_directory=True)
            result = subprocess.run([sys.executable, "-I", "-B", str(fresh / "run_graphics_probe.py"),
                "--mode", "describe", "--output", str(root / "linked-parent/evidence")],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Symbolic", json.loads(result.stdout)["reason"])
            self.assertFalse((root / "real-parent/evidence").exists())

    @unittest.skipUnless(sys.platform == "linux", "Actual Linux process/permission fixture")
    def test_bounded_native_process_timeout_and_output_cap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = launcher.run_bounded([sys.executable, "-c", "import time; time.sleep(120)"], cwd=root, env=os.environ, timeout=0.1)
            self.assertTrue(result.timed_out)
            self.assertEqual(result.returncode, 124)
            result = launcher.run_bounded([sys.executable, "-c", "import os; os.write(1,b'x'*100000)"],
                                          cwd=root, env=os.environ, timeout=3, maximum=1024)
            self.assertEqual(len(result.stdout), 1024)
            self.assertEqual(result.returncode, 1)
            self.assertTrue(result.cleanup_errors)

    @unittest.skipUnless(sys.platform == "linux", "Actual Linux process/permission fixture")
    def test_descendant_holding_stdout_is_retired(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = "import os,time; p=os.fork(); print(p,flush=True) if p else time.sleep(120)"
            result = launcher.run_bounded([sys.executable, "-c", script], cwd=root, env=os.environ, timeout=3)
            pid = int(result.stdout.strip())
            try:
                self.assertEqual(result.returncode, 0)
                self.assertFalse(result.timed_out)
                deadline = time.monotonic() + 1
                path = Path(f"/proc/{pid}/stat")
                while path.exists() and path.read_text().split(")", 1)[1].split()[0] != "Z" and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(not path.exists() or path.read_text().split(")", 1)[1].split()[0] == "Z")
            finally:
                retire_fixture(pid, script)

    @unittest.skipUnless(sys.platform == "linux" and sys.version_info >= (3, 12), "Actual standalone Python 3.12+ fixture")
    def test_isolated_standalone_launcher_relocation_and_fresh_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package_dir = root / "package space 測試"
            package.publish(package_dir, *fixture_payload())
            output = root / "evidence space 測試"
            command = [sys.executable, "-I", "-B", str(package_dir / "run_graphics_probe.py"),
                       "--mode", "describe", "--output", str(output)]
            result = subprocess.run(command, cwd=root, env={"PATH": "/usr/bin:/bin"}, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            summary = json.loads((output / "summary.json").read_text())
            self.assertEqual(summary["status"], "PASS")
            self.assertEqual(summary["qualification"], "NOT_RUN")
            self.assertFalse(list(package_dir.rglob("*.pyc")))
            original = (output / "summary.json").read_bytes()
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 1)
            self.assertEqual((output / "summary.json").read_bytes(), original)

    @unittest.skipUnless(sys.platform == "linux" and sys.version_info >= (3, 12), "Actual Linux standalone CLI fixture")
    def test_cli_rejects_unrequested_workload_and_loader_delimiters(self):
        for name, extra in (("ordinary", ["--present-seconds", "600"]), ("colon:path", []),
                            ("semicolon;path", []), ("dollar$ORIGIN", [])):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                directory = root / name
                package.publish(directory, *fixture_payload())
                command = [sys.executable, "-I", "-B", str(directory / "run_graphics_probe.py"),
                           "--mode", "describe", "--output", str(root / "out"), *extra]
                result = subprocess.run(command, capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(json.loads(result.stdout)["status"], "FAIL")
                self.assertFalse((root / "out").exists())

    @unittest.skipUnless(sys.platform == "linux" and sys.version_info >= (3, 12), "Actual Linux standalone CLI fixture")
    def test_cli_normalizes_own_directory_before_rejecting_internal_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "package"
            package.publish(directory, *fixture_payload())
            (directory / "unused").mkdir()
            command = [sys.executable, "-I", "-B", str(directory / "unused/../run_graphics_probe.py"),
                       "--mode", "describe", "--output", str(directory / "evidence")]
            result = subprocess.run(command, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("outside", json.loads(result.stdout)["reason"])
            self.assertFalse((directory / "evidence").exists())

    @unittest.skipUnless(sys.platform == "linux" and sys.version_info >= (3, 12), "Actual Linux standalone CLI fixture")
    def test_root_gpu_operation_is_not_run_and_does_not_start_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "package"
            package.publish(directory, *fixture_payload())
            command = ["run_graphics_probe.py", "--mode", "cases", "--device", "a" * 32, "--output", str(root / "evidence")]
            with patch.object(launcher, "__file__", str(directory / "run_graphics_probe.py")), \
                    patch.object(sys, "argv", command), patch.object(os, "geteuid", return_value=0), \
                    patch.object(launcher, "run_bounded") as run, redirect_stdout(io.StringIO()):
                self.assertEqual(launcher.main(), 3)
            run.assert_not_called()
            self.assertEqual(json.loads((root / "evidence/summary.json").read_text())["status"], "NOT_RUN")

    @unittest.skipUnless(sys.platform == "linux" and sys.version_info >= (3, 12), "Actual Linux standalone CLI fixture")
    def test_malformed_native_reports_and_unknown_exit_cannot_pass(self):
        for native, code in (('{"status":"PASS","status":"PASS"}', 0), ('{"number":NaN}', 0),
                             ('{"number":1e999}', 0), ('{}', 17)):
            with self.subTest(native=native, code=code), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                directory = root / "package"
                payload, manifest = fixture_payload()
                payload["bin/GraphicsProbe"] = ("#!/usr/bin/python3\nimport sys\nprint(" + repr(native)
                                               + f")\nsys.exit({code})\n").encode()
                package.publish(directory, payload, manifest)
                result = subprocess.run([sys.executable, "-I", "-B", str(directory / "run_graphics_probe.py"),
                    "--mode", "describe", "--output", str(root / "evidence")], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                summary = json.loads((root / "evidence/summary.json").read_text())
                self.assertEqual(summary["status"], "FAIL")
                self.assertTrue(summary["errors"])

    def launcher_interruption(self, signum):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "package"
            payload, manifest = fixture_payload()
            payload["bin/GraphicsProbe"] = b"#!/usr/bin/python3\nimport os,time\nfrom pathlib import Path\np=os.fork()\nif p:\n Path('child.tmp').write_text(str(p))\n os.replace('child.tmp','child.pid')\ntime.sleep(120)\n"
            package.publish(directory, payload, manifest)
            output = root / "evidence"
            command = [sys.executable, "-I", "-B", str(directory / "run_graphics_probe.py"),
                       "--mode", "describe", "--output", str(output)]
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            pid_path = output / "child.pid"
            try:
                deadline = time.monotonic() + 5
                while not pid_path.exists() and time.monotonic() < deadline and process.poll() is None:
                    time.sleep(0.01)
                self.assertTrue(pid_path.exists(), "Synthetic native child did not publish its PID")
                pid = int(pid_path.read_text())
                process.send_signal(signum)
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 1, stdout + stderr)
                summary = json.loads((output / "summary.json").read_text())
                self.assertTrue(any("interrupted" in error for error in summary["cleanup_errors"]))
                path = Path(f"/proc/{pid}/stat")
                deadline = time.monotonic() + 1
                while path.exists() and path.read_text().split(")", 1)[1].split()[0] != "Z" and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(not path.exists() or path.read_text().split(")", 1)[1].split()[0] == "Z")
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        # Test-failure cleanup owns these exact fixture PIDs.
                        if pid_path.exists():
                            retire_fixture(int(pid_path.read_text()), str(directory / "bin/GraphicsProbe"))
                        process.kill()
                        process.communicate(timeout=5)
                if pid_path.exists():
                    retire_fixture(int(pid_path.read_text()), str(directory / "bin/GraphicsProbe"))

    @unittest.skipUnless(sys.platform == "linux" and sys.version_info >= (3, 12), "Actual Linux standalone interruption fixture")
    def test_sigterm_launcher_interrupt_retires_native_descendant(self):
        self.launcher_interruption(signal.SIGTERM)

    @unittest.skipUnless(sys.platform == "linux" and sys.version_info >= (3, 12), "Actual Linux standalone interruption fixture")
    def test_sigint_launcher_interrupt_retires_native_descendant(self):
        self.launcher_interruption(signal.SIGINT)


if __name__ == "__main__":
    unittest.main()
