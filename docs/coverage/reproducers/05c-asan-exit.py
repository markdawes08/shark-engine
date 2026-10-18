"""Capture-ready diagnosis of the retained 05c ASan post-close timeout.

Run with the x64 MSVC developer wrapper. The default selfcheck never creates a
graphics device. Native mode runs at most64 children in the historical NV-four,
Intel-four fault order, stopping at the first slow or unexpected child. Native
deadlines remain30 seconds. No result from this tool qualifies REQ-05.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, replace
from datetime import datetime, timezone
import importlib.util
import inspect
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tarfile
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
from graphics_probe import check_layer_process, layer_environment, sha256, validation_tools
from graphics_device_smoke import source_digest
from presentation_faults import NATIVE_FAULTS, inspect_result, source_inputs
from process_runner import run_process
from windows_unelevated import run_standard

GPUS = ("863d6f59a51894ab8b4829aa9460c78d", "868088a7040000000002000000000000")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def process_identity(pid):
    """Read identity/status for a PID obtained from our own Popen/event chain."""
    import ctypes
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (w.DWORD, w.BOOL, w.DWORD)
    kernel.OpenProcess.restype = w.HANDLE
    kernel.GetProcessTimes.argtypes = (w.HANDLE, ctypes.POINTER(w.FILETIME), ctypes.POINTER(w.FILETIME), ctypes.POINTER(w.FILETIME), ctypes.POINTER(w.FILETIME))
    kernel.GetExitCodeProcess.argtypes = (w.HANDLE, ctypes.POINTER(w.DWORD))
    kernel.QueryFullProcessImageNameW.argtypes = (w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD))
    kernel.CloseHandle.argtypes = (w.HANDLE,)
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return {"pid": pid, "error": ctypes.get_last_error()}
    try:
        created, exited, system, user = (w.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)):
            raise ctypes.WinError(ctypes.get_last_error())
        code = w.DWORD()
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        path = ctypes.create_unicode_buffer(32768)
        size = w.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        return {"pid": pid, "creation_time": (created.dwHighDateTime << 32) | created.dwLowDateTime,
                "image": path.value, "exit_code": code.value}
    finally:
        kernel.CloseHandle(handle)


def diagnostic_event(kind, subject=None, identity=None, **extra):
    import time
    identity = process_identity(subject or os.getpid()) if identity is None else identity
    if "error" in identity:
        raise RuntimeError("Owned process identity unavailable")
    with open(os.environ["SHARK_EXIT_EVENTS"], "a", encoding="utf-8") as stream:
        stream.write(json.dumps({"kind": kind, "observer_pid": os.getpid(), "monotonic": time.monotonic(),
                                 "identity": identity, **extra}) + "\n")
        stream.flush()


def diagnostic_worker_exit():
    diagnostic_event("worker_atexit_enter")
    if os.environ.get("SHARK_EXIT_WORKER_STALL") == "selfcheck":
        import threading
        threading.Event().wait(60)
    diagnostic_event("worker_atexit_leave")


def substitute(text, before, after):
    require(text.count(before) == 1, f"Nonunique instrumentation site: {before}")
    return text.replace(before, after)


def instrument(source):
    text = '#include "' + (HERE / "05c-exit-markers.hpp").as_posix() + '"\n' + source
    for before, after in [
        ('int native_fault(std::string_view fault, const std::string& uuid) {',
         'int native_fault(std::string_view fault, const std::string& uuid) {\n    DiagnosticScopeExit exit_scope;\n    diagnostic_marker("native_function_entered");'),
        ('    owner.reset();\n    return 1; // Expected failure remains nonzero even though cleanup succeeded.',
         '    diagnostic_marker("before_owner_reset");\n    owner.reset();\n    diagnostic_marker("after_owner_reset");\n    return 1; // Expected failure remains nonzero even though cleanup succeeded.'),
        ('    std::set_terminate(observe_termination);',
         '    std::set_terminate(observe_termination);\n    diagnostic_register_exit();\n    if (argc == 2 && std::string_view(argv[1]) == "--diagnostic-cpu-stall") {\n        diagnostic_cpu_stall = true;\n        diagnostic_marker("cpu_main_return");\n        return 1;\n    }'),
        ('                return native_fault(fault, uuid);',
         '                const auto status = native_fault(fault, uuid);\n                diagnostic_marker("main_return");\n                return status;')]:
        text = substitute(text, before, after)
    return text


def write_runner(output):
    text = (ROOT / "tools/process_runner.py").read_text(encoding="utf-8")
    helpers = "\n\n".join(inspect.getsource(function) for function in (process_identity, diagnostic_event, diagnostic_worker_exit))
    text = substitute(text, '\ndef _worker() -> int:', '\n' + helpers + '\ndef _worker() -> int:')
    text = substitute(text, '                job.assign(process.pid)',
                      '                diagnostic_event("worker_launched", subject=process.pid)\n                job.assign(process.pid)')
    text = substitute(text, '    try:\n        return subprocess.call(json.loads(sys.argv[2]), stdin=subprocess.DEVNULL)',
        '    try:\n        import atexit\n        atexit.register(diagnostic_worker_exit)\n        diagnostic_event("worker_started")\n        child = subprocess.Popen(json.loads(sys.argv[2]), stdin=subprocess.DEVNULL)\n        child_identity = process_identity(child.pid)\n        diagnostic_event("child_started", identity=child_identity)\n        result = child.wait()\n        diagnostic_event("child_exited", identity=child_identity, returncode=result)\n        return result')
    path = output / "observed_runner.py"
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def prepare(args, output):
    build = args.build_dir.resolve()
    cache = (build / "CMakeCache.txt").read_text()
    require("SHARK_SANITIZER:STRING=address" in cache and "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo" in cache,
            "Requires an existing windows-asan RelWithDebInfo build")
    snapshot = output / "source-snapshot"
    revision = None
    if args.source_revision:
        require(re.fullmatch(r"[0-9a-fA-F]{7,40}", args.source_revision) is not None, "Source revision must be an immutable abbreviated or full commit ID")
        resolved = run_process(["git", "rev-parse", "--verify", args.source_revision], cwd=ROOT, timeout=5)
        require(resolved.returncode == 0 and not resolved.timed_out, "Cannot resolve source revision")
        revision = resolved.stdout.strip()
        object_type = run_process(["git", "cat-file", "-t", revision], cwd=ROOT, timeout=5)
        require(object_type.returncode == 0 and not object_type.timed_out and object_type.stdout.strip() == "commit", "Source revision must name a commit")
        tree = run_process(["git", "ls-tree", "-r", "--name-only", revision], cwd=ROOT, timeout=5)
        require(tree.returncode == 0 and not tree.timed_out, "Cannot read source revision inventory")
        required = {"CMakeLists.txt", "dependencies.lock.json", "engine/CMakeLists.txt", "tests/CMakeLists.txt",
                    "tests/presentation_faults.cpp", "probes/graphics/report.hpp"}
        paths = sorted(name for name in tree.stdout.splitlines() if name in required
                       or (name.startswith(("engine/core/", "engine/client/")) and (Path(name).suffix in (".cpp", ".hpp", ".manifest") or Path(name).name == "CMakeLists.txt"))
                       or (Path(name).parent.as_posix() == "cmake" and Path(name).suffix == ".cmake"))
        require(required.issubset(paths), "Revision lacks required diagnostic source inputs")
        archive = output / "source-revision.tar"
        archived = run_process(["git", "archive", "--format=tar", "--output=" + str(archive), revision, *paths], cwd=ROOT, timeout=10)
        require(archived.returncode == 0 and not archived.timed_out, "Cannot archive source revision")
        with tarfile.open(archive) as stream:
            for member in stream.getmembers():
                if not member.isfile():
                    continue
                require(member.name in paths, "Unexpected archived source path")
                target = snapshot / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(stream.extractfile(member).read())
        current_inputs = {name: sha256(snapshot / name) for name in paths}
    else:
        current_inputs = source_inputs()
        for name, expected in current_inputs.items():
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, target)
            require(sha256(target) == expected, "Source changed while taking the diagnostic snapshot")
    digest = source_digest(current_inputs)
    report = {"schema_version": 1, "qualification": "NOT_RUN", "status": "PREPARING", "mode": args.mode,
              "source_inputs": current_inputs, "source_sha256": digest, "source_revision": revision, "phases": [], "variants": [], "errors": [],
              "started_utc": datetime.now(timezone.utc).isoformat(), "native_deadline_seconds": 30,
              "native_capture_threshold_seconds": 8, "maximum_native_children": 64,
              "ordering": "Eight blocks of NVIDIA four faults then Intel four faults; no stress soak was present in the original regression sequence",
              "limitations": ["Original ASAN_OPTIONS and complete host activity were not captured; exact ambient reconstruction is impossible.",
                              "Instrumentation and successful repetitions do not establish a causal fix.",
                              "Only newly added late marker functions omit ASan instrumentation; existing fixture and engine instrumentation is unchanged."]}
    save(output / "summary.json", report)
    report["frozen_source_sha256"] = {str(snapshot / name): value for name, value in current_inputs.items()}
    libraries = [build / name for name in ("engine/client/EngineClient.lib", "engine/core/EngineCore.lib",
                 "_deps/glfw-build/src/glfw3.lib", "_deps/nvrhi-build/nvrhi_vk.lib", "_deps/nvrhi-build/nvrhi.lib",
                 "_deps/vulkanloader-build/loader/vulkan-1.lib")]
    files = [Path(__file__), HERE / "05c-exit-capture.cpp", HERE / "05c-exit-markers.hpp", ROOT / "tests/presentation_faults.cpp",
             ROOT / "engine/client/windows.manifest", ROOT / "dependencies.lock.json", build / "build.ninja", build / "CMakeCache.txt",
             build / "bin/PresentationFaults.exe", build / "bin/vulkan-1.dll", *libraries,
             *[ROOT / "tools" / name for name in ("process_runner.py", "presentation_faults.py", "graphics_device_smoke.py", "graphics_probe.py", "windows_unelevated.py", "toolchain_report.py")]]
    variants = [("current", snapshot / "tests/presentation_faults.cpp", build / "bin/PresentationFaults.exe", digest)]
    if args.include_prior:
        prior_receipt = ROOT / "out/ci/05c-faults-regression/windows-asan/868088a7040000000002000000000000/summary.json"
        prior_source = ROOT / "out/05c-work/fault-observer-amendment/prior/presentation_faults.cpp"
        prior_executable = prior_source.parent / "windows-asan/PresentationFaults.exe"
        prior = json.loads(prior_receipt.read_text())
        require(sha256(prior_source) == prior["input_sha256"]["tests/presentation_faults.cpp"]
                and sha256(prior_executable) == prior["executable_sha256"], "Preserved original identity differs")
        require({k: v for k, v in prior["input_sha256"].items() if k != "tests/presentation_faults.cpp"}
                == {k: v for k, v in current_inputs.items() if k != "tests/presentation_faults.cpp"},
                "Other production inputs differ from original; prior/current comparison is unavailable")
        variants.append(("prior", prior_source, prior_executable, prior["source_sha256"]))
        files.extend([prior_receipt, prior_source, prior_executable, prior_executable.parent / "vulkan-1.dll"])
        report["original_failure"] = {"path": str(prior_receipt), "sha256": sha256(prior_receipt)}
    report["input_sha256"] = {str(path.resolve()): sha256(path) for path in files}
    compiler = shutil.which("cl")
    require(compiler is not None, "Use tools/windows_dev.ps1 to select x64 MSVC")
    report["compiler"] = {"path": compiler, "sha256": sha256(Path(compiler))}
    ninja = (build / "build.ninja").read_text()
    block = ninja.split('build tests\\CMakeFiles\\PresentationFaults.dir\\presentation_faults.cpp.obj:', 1)[1].split('\n\n', 1)[0]
    fields = dict(re.findall(r'^  (\w+) = (.*)$', block, re.MULTILINE))
    includes = []
    for part in fields["INCLUDES"].split():
        if part.startswith("-I" + ROOT.as_posix()) and not part.startswith("-I" + (ROOT / "build").as_posix()):
            part = part.replace(ROOT.as_posix(), snapshot.as_posix(), 1)
        includes.append(part)
    require("/fsanitize=address" in fields["FLAGS"] and digest in fields["DEFINES"], "ASan fixture build metadata is stale")
    def compile(name, command):
        result = run_process(command, cwd=ROOT, timeout=120)
        save(output / f"{name}-build.json", asdict(result))
        report["phases"].append({"name": name, **asdict(result)})
        save(output / "summary.json", report)
        require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors, f"Build {name} failed; retained logs contain details")
    for label, original, existing, source_hash in variants:
        report["variants"].append({"name": label + "_original", "executable": str(existing), "source_sha256": source_hash, "instrumented": False})
        copy = output / f"{label}_instrumented.cpp"
        copy.write_text(instrument(original.read_text(encoding="utf-8")), encoding="utf-8", newline="\n")
        executable = output / f"{label}_instrumented.exe"
        defines = fields["DEFINES"].replace(digest, source_hash)
        command = [compiler, "/nologo", "/TP", *[part.replace('\\"', '"') for part in defines.split()],
                   *includes, *fields["FLAGS"].split(), "/Fo" + str(copy.with_suffix(".obj")),
                   "/Fd" + str(output / f"{label}-compiler.pdb"), str(copy), "/Fe" + str(executable), "/link", "/machine:x64", "/DEBUG", "/INCREMENTAL:NO",
                   "/MANIFEST:EMBED", "/MANIFESTINPUT:" + str(ROOT / "engine/client/windows.manifest"),
                   *[str(path) for path in libraries], "bcrypt.lib", "kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib", "shell32.lib", "ole32.lib", "oleaut32.lib", "uuid.lib", "comdlg32.lib", "advapi32.lib"]
        compile(label, command)
        report["variants"].append({"name": label + "_instrumented", "executable": str(executable), "source_sha256": source_hash,
                                   "instrumented": True, "copy_source_sha256": sha256(copy), "original_source_sha256": sha256(original)})
    capture = output / "CaptureExit.exe"
    compile("capture", [compiler, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/permissive-", "/MD", "/utf-8",
                        str(HERE / "05c-exit-capture.cpp"), "/Fo" + str(output / "capture.obj"), "/Fe" + str(capture), "/link", "dbghelp.lib", "advapi32.lib"])
    shutil.copy2(build / "bin/vulkan-1.dll", output / "vulkan-1.dll")
    runner = write_runner(output)
    report["generated_sha256"] = {str(path): sha256(path) for path in [capture, runner, output / "vulkan-1.dll", *output.glob("*_instrumented.cpp"), *output.glob("*_instrumented.exe")]}
    for variant in report["variants"]:
        variant["executable_sha256"] = sha256(Path(variant["executable"]))
    report["sanitizer_environment"] = {key: value for key, value in os.environ.items() if key.startswith(("ASAN_", "LSAN_", "UBSAN_"))}
    report["status"] = "PREPARED"
    require(revision is not None or source_inputs() == current_inputs, "Source changed during diagnostic compilation")
    save(output / "summary.json", report)
    return report


def owned_identities(events):
    identities = []
    seen = set()
    for kind in ("child_started", "worker_started", "worker_launched"):
        for row in events:
            if row["kind"] != kind:
                continue
            identity = row["identity"]
            key = (identity["pid"], identity["creation_time"])
            if key not in seen:
                identities.append(identity)
                seen.add(key)
    return identities


def capture_slow(output, directory, done, threshold, captures, errors):
    try:
        if done.wait(threshold):
            return
        events = [json.loads(line) for line in (directory / "events.jsonl").read_text().splitlines()]
        states = []
        for identity in owned_identities(events):
            now = process_identity(identity["pid"])
            states.append({"recorded": identity, "observed": now})
        save(directory / "slow-processes.json", states)
        for state in states:
            if done.is_set():
                break
            identity, now = state["recorded"], state["observed"]
            if now.get("creation_time") != identity["creation_time"] or now.get("exit_code") != 259:
                continue
            dump = directory / f"{identity['pid']}.dmp"
            result = run_process([str(output / "CaptureExit.exe"), str(identity["pid"]), str(identity["creation_time"]), str(dump)], cwd=output, timeout=5)
            capture = {"identity": identity, "dump": str(dump), **asdict(result)}
            if dump.exists():
                capture["dump_sha256"] = sha256(dump)
            captures.append(capture)
            save(directory / f"{identity['pid']}-capture.json", capture)
    except Exception as error:
        errors.append(f"Observer: {type(error).__name__}: {error}")


def observe_child(output, name, command, environment, timeout, threshold):
    directory = output / name
    directory.mkdir()
    events = directory / "events.jsonl"
    events.touch()
    os.environ["SHARK_EXIT_EVENTS"] = str(events)
    environment = dict(environment, SHARK_EXIT_EVENTS=str(events))
    specification = importlib.util.spec_from_file_location("observed_runner", output / "observed_runner.py")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    done, captures, errors = threading.Event(), [], []
    watcher = threading.Thread(target=capture_slow, args=(output, directory, done, threshold, captures, errors), daemon=True)
    watcher.start()
    started = time.monotonic()
    result = module.run_process(command, cwd=Path(command[0]).parent, env=environment, timeout=timeout)
    done.set()
    watcher.join(timeout=20)
    if watcher.is_alive():
        errors.append("Observer did not finish within its bounded join")
    record = {"name": name, "elapsed_seconds": time.monotonic() - started, "deadline_seconds": timeout,
              "capture_threshold_seconds": threshold, **asdict(result), "captures": captures, "observer_errors": errors,
              "events": [json.loads(line) for line in events.read_text().splitlines()]}
    (directory / "stdout.jsonl").write_text(result.stdout, encoding="utf-8")
    (directory / "stderr.log").write_text(result.stderr, encoding="utf-8")
    save(directory / "summary.json", record)
    return result, record


def offline_stacks(args, output, runs):
    candidates = [args.debugger.resolve()] if args.debugger else sorted(Path("C:/Program Files/WindowsApps").glob("Microsoft.WinDbg_*/amd64/cdb.exe"))
    debugger = candidates[-1] if candidates else None
    evidence = {"available": debugger is not None, "analyses": []}
    if not debugger:
        return evidence
    evidence.update(path=str(debugger), sha256=sha256(debugger))
    environment = dict(os.environ)
    for key in ("_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH", "_NT_EXECUTABLE_IMAGE_PATH"):
        environment.pop(key, None)
    local = str(output) + ";" + str(args.build_dir.resolve() / "bin")
    for row in runs:
        for capture in row["captures"]:
            dump = Path(capture["dump"])
            if not dump.is_file() or not dump.stat().st_size:
                continue
            result = run_process([str(debugger), "-y", local, "-i", local, "-z", str(dump), "-c", ".symopt+0x80200;~* kP;lm;q"], cwd=output, env=environment, timeout=30)
            log = dump.with_suffix(".stacks.log")
            log.write_text(result.stdout + result.stderr, encoding="utf-8")
            evidence["analyses"].append({"dump": str(dump), "dump_sha256": sha256(dump), "log": str(log), "log_sha256": sha256(log), **asdict(result)})
    return evidence


def worker(args, output):
    report = json.loads((output / "summary.json").read_text())
    require(report["status"] == "PREPARED", "Worker accepts a fresh prepared diagnostic only")
    require(all(sha256(Path(path)) == digest for key in ("input_sha256", "generated_sha256", "frozen_source_sha256")
                for path, digest in report[key].items()), "Prepared diagnostic identity changed before launch")
    report["status"] = "RUNNING"
    save(output / "summary.json", report)
    report["process_security"] = check_layer_process()
    report["runs"] = []
    current = next(row for row in report["variants"] if row["name"] == "current_instrumented")
    environment = dict(os.environ)
    for key in list(environment):
        if key.startswith("SHARK_EXIT_"):
            del environment[key]
    if args.mode == "selfcheck":
        for name, command, extra in [
            ("native_atexit_stall", [current["executable"], "--diagnostic-cpu-stall"], {}),
            ("worker_atexit_stall", [current["executable"], "--describe"], {"SHARK_EXIT_WORKER_STALL": "selfcheck"})]:
            result, record = observe_child(output, name, command, dict(environment, **extra), 5, 1)
            report["runs"].append(record)
            save(output / "summary.json", report)
            require(result.timed_out and result.returncode == 124 and not result.cleanup_errors and not record["observer_errors"], "Controlled stall did not preserve the expected finite deadline")
            require(record["captures"] and all(item.get("dump_sha256") and item["returncode"] == 0 and not item["timed_out"] for item in record["captures"]), "Controlled stall lacks valid bound-process captures")
            exited = [event for event in record["events"] if event["kind"] == "child_exited"]
            require(bool(exited) == (name == "worker_atexit_stall"), "Observer confused native and worker exit boundaries")
            require(("cpu_exit_stall" in result.stdout) == (name == "native_atexit_stall"), "Wrong controlled native shutdown stage")
        # A mismatched creation time must be rejected before producing a dump.
        identity = process_identity(os.getpid())
        mismatch = output / "identity-mismatch.dmp"
        rejected = run_process([str(output / "CaptureExit.exe"), str(identity["pid"]), str(identity["creation_time"] + 1), str(mismatch)], cwd=output, timeout=5)
        report["identity_negative_check"] = asdict(rejected)
        require(rejected.returncode == 4 and not rejected.timed_out and not mismatch.exists(), "Observer accepted the wrong process creation time")
        events = report["runs"][0]["events"]
        selected = owned_identities(events)
        duplicated = events + events + [dict(kind="worker_launched", identity=item) for item in selected]
        require(owned_identities(duplicated) == selected, "Duplicate process events changed capture targets")
        require(len(selected) == len({(item["pid"], item["creation_time"]) for item in selected}), "Capture targets are not unique")
        for row in report["runs"]:
            keys = [(item["identity"]["pid"], item["identity"]["creation_time"]) for item in row["captures"]]
            require(len(keys) == len(set(keys)), "Owned process was captured more than once")
        report["duplicate_identity_check"] = {"status": "PASS", "duplicated_events": len(duplicated), "distinct_targets": len(selected)}
        report["status"] = "DIAGNOSTIC_SELFTEST_PASS"
    else:
        lock = sha256(ROOT / "dependencies.lock.json")
        validation = validation_tools(args.validation_manifest.resolve(), lock)
        report["validation_tools"] = {"manifest": str(args.validation_manifest.resolve()), "sha256": sha256(args.validation_manifest), "receipt": validation}
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Required core/synchronization/NVRHI validation; no overrides.\n")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, report["layer_environment"] = layer_environment(validation, settings, implicit)
        require(not report["layer_environment"]["driver_selection"], "Ambient driver override would change the historical workload")
        environment.pop("SHARK_EXIT_WORKER_STALL", None)
        stop = False
        for block in range(8):
            variant = report["variants"][block % len(report["variants"])]
            for gpu in GPUS:
                for fault in NATIVE_FAULTS:
                    name = f"{len(report['runs']) + 1:02}-{variant['name']}-{gpu[:4]}-{fault}"
                    result, record = observe_child(output, name, [variant["executable"], "--native-fault", fault, "--device", gpu], environment, 30, 8)
                    record.update(block=block + 1, variant=variant["name"], device=gpu, fault=fault)
                    try:
                        rows = [json.loads(line) for line in result.stdout.splitlines()]
                        require(all(isinstance(row, dict) for row in rows), "Native trace row is not an object")
                        record["markers"] = [row for row in rows if row.get("kind") == "exit_marker"]
                        stripped = "\n".join(json.dumps(row) for row in rows if row.get("kind") != "exit_marker") + "\n"
                        _, record["protocol_errors"] = inspect_result(replace(result, stdout=stripped), "native", fault, gpu, lock, variant["source_sha256"])
                    except (ValueError, TypeError) as error:
                        record["markers"] = []
                        record["protocol_errors"] = [f"Malformed native trace: {error}"]
                    report["runs"].append(record)
                    save(output / "summary.json", report)
                    print(json.dumps({"child": len(report["runs"]), "variant": variant["name"], "gpu": gpu, "fault": fault, "exit": result.returncode, "timed_out": result.timed_out}), flush=True)
                    if record["captures"] or record["observer_errors"] or record["protocol_errors"]:
                        report["stopped_at_first_observation"] = name
                        stop = True
                        break
                if stop:
                    break
            if stop:
                break
        require(validation_tools(args.validation_manifest.resolve(), lock) == validation, "Validation tools changed during execution")
        report["status"] = "CAPTURED_OBSERVATION" if any(row["captures"] for row in report["runs"]) else "UNRESOLVED"
    report["debugger"] = offline_stacks(args, output, report["runs"])
    report["identities_unchanged"] = all(sha256(Path(path)) == digest for key in ("input_sha256", "generated_sha256", "frozen_source_sha256") for path, digest in report[key].items())
    live = source_inputs()
    report["live_source_changes_after_snapshot"] = [{"path": name, "before": report["source_inputs"].get(name), "after": live.get(name)}
        for name in sorted(set(live) | set(report["source_inputs"])) if live.get(name) != report["source_inputs"].get(name)]
    require(report["identities_unchanged"], "Source/tool/binary inputs changed during diagnosis")
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    save(output / "summary.json", report)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("selfcheck", "native"), default="selfcheck")
    parser.add_argument("--include-prior", action="store_true", help="Also use the retained pre-amendment source/binary; requires local archived evidence")
    parser.add_argument("--source-revision", help="Read-only Git snapshot matching the existing binaries while separate live source work proceeds")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/windows-asan")
    parser.add_argument("--validation-manifest", type=Path, default=ROOT / "out/05a-work/validation-tools/summary.json")
    parser.add_argument("--debugger", type=Path, help="Installed cdb.exe for bounded offline local-symbol analysis")
    parser.add_argument("--internal-worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    output = args.output.resolve()
    if sys.platform != "win32" or not output.is_relative_to((ROOT / "out").resolve()) or output == (ROOT / "out").resolve():
        parser.error("Requires native Windows and a fresh directory beneath repository out/")
    if args.internal_worker:
        if not (output / "summary.json").is_file() or json.loads((output / "summary.json").read_text()).get("status") != "PREPARED":
            parser.error("Internal worker requires a fresh prepared diagnostic; previous evidence is preserved")
        try:
            return worker(args, output)
        except Exception as error:
            report = json.loads((output / "summary.json").read_text())
            report["errors"].append(f"{type(error).__name__}: {error}")
            report["status"] = "DIAGNOSTIC_FAILURE"
            save(output / "summary.json", report)
            raise
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Existing evidence is never overwritten: {error}")
    try:
        prepare(args, output)
        command = [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--internal-worker"]
        if not args.debugger:
            debuggers = sorted(Path("C:/Program Files/WindowsApps").glob("Microsoft.WinDbg_*/amd64/cdb.exe"))
            if debuggers:
                command.extend(["--debugger", str(debuggers[-1])])
        result, security = run_standard(command, cwd=ROOT, timeout=900)
        save(output / "token.json", {"security": security, "result": asdict(result)})
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors, "Diagnostic worker failed; raw evidence retained")
        summary = json.loads((output / "summary.json").read_text())
        print(json.dumps({"status": summary["status"], "qualification": "NOT_RUN", "summary": str(output / "summary.json")}))
        return 0
    except Exception as error:
        print(f"Diagnostic failure: {error}", file=sys.stderr)
        if (output / "summary.json").is_file():
            report = json.loads((output / "summary.json").read_text())
            report["errors"].append(f"{type(error).__name__}: {error}")
            report["status"] = "DIAGNOSTIC_FAILURE"
            save(output / "summary.json", report)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
