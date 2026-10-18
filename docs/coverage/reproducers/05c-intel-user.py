"""Bounded owned-hook control or forty raw Intel Vulkan lifetimes; never qualification.

CDB tracing is optional and requires an explicitly supplied installed debugger and
existing local OS symbol cache. No production libraries or foreign-handle cleanup.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from graphics_probe import Unavailable, layer_environment, unique_object, validation_tools
from process_runner import run_process

INTEL = "868088a7040000000002000000000000"


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_control(text):
    rows = [json.loads(line, object_pairs_hook=unique_object) for line in text.splitlines()]
    require(len(rows) == 121 and rows[-1] == {"complete": True, "qualification": "NOT_RUN"},
            "Owned-hook control incomplete")
    offset = 0

    def sample(phase, cycle):
        nonlocal offset
        row = rows[offset]
        offset += 1
        require(set(row) == {"phase", "cycle", "thread", "user", "gdi"}
                and row["phase"] == phase and type(row["cycle"]) is int and row["cycle"] == cycle
                and all(type(row[key]) is int and 0 <= row[key] <= 0xffffffff
                        for key in ("thread", "user", "gdi")), "Invalid control sample")

    sample("initial", 0)
    for cycle in range(1, 21):
        sample("owned", cycle)
        if cycle % 2 == 0:
            sample("target_exited", cycle)
        for hook_id in (4, 3, 12):
            row = rows[offset]
            offset += 1
            require(set(row) == {"phase", "cycle", "hook_id", "target", "exit_first", "result", "error"}
                    and row["phase"] == "unhook" and type(row["cycle"]) is int and row["cycle"] == cycle
                    and type(row["hook_id"]) is int and row["hook_id"] == hook_id
                    and row["exit_first"] is (cycle % 2 == 0)
                    and all(type(row[key]) is int and 0 <= row[key] <= 0xffffffff
                            for key in ("target", "result", "error"))
                    and row["target"] > 0 and (cycle % 2 == 0 or row["result"] != 0),
                    "Invalid owned-hook removal observation")
        sample("closed", cycle)
    for ms in (0, 16, 50, 100, 250, 500, 1000, 2000, 5000):
        sample("settled", ms)
    require(offset == len(rows) - 1, "Unexpected control suffix")
    return rows


def debugger_commands(output):
    directory = output / "commands"
    directory.mkdir()
    initial = ".reload /f ntdll.dll\ndt ntdll!_TEB LastErrorValue\n"
    for api in ("SetWindowsHookExA", "SetWindowsHookExW", "UnhookWindowsHookEx", "CreateThread", "CreateRemoteThreadEx"):
        module = "user32" if "Hook" in api else ("kernel32" if api == "CreateThread" else "kernelbase")
        entry, returned = directory / f"entry-{api}.txt", directory / f"return-{api}.txt"
        commands = f'.printf "USER_RETURN {api} tid=%x rsp=%p result=%p caller=%p\\n", @$tid, @rsp, @rax, @rip\n'
        if api == "UnhookWindowsHookEx":
            commands += ".if (@rax == 0) { !gle }\n"
        if api.startswith("Create"):
            # x64 arguments six/eight, after the return address has been popped.
            offset = "0x28" if api == "CreateThread" else "0x38"
            commands += f'.if (poi(@rsp+{offset}) != 0) {{ .printf "THREAD_TARGET api={api} id=%x\\n", dwo(poi(@rsp+{offset})) }}\n'
        returned.write_text(commands + "gc\n", encoding="ascii")
        entry.write_text(f'.printf "USER_ENTRY {api} tid=%x rsp=%p a0=%p a1=%p a2=%p a3=%p caller=%p\\n", @$tid, @rsp, @rcx, @rdx, @r8, @r9, poi(@rsp)\nk10\n~.bp /1 poi(@rsp) "$$<{returned.as_posix()}"\ngc\n', encoding="ascii")
        initial += f'bu {module}!{api} "$$<{entry.as_posix()}"\n'
    exit_file = directory / "exit-thread.txt"
    exit_file.write_text('.printf "THREAD_EXIT_ENTRY tid=%x status=%p caller=%p\\n", @$tid, @rcx, poi(@rsp)\nk10\ngc\n', encoding="ascii")
    initial += f'bu ntdll!RtlExitUserThread "$$<{exit_file.as_posix()}"\ng\n'
    (directory / "initial.txt").write_text(initial, encoding="ascii")
    return directory / "initial.txt"


def inspect_debugger(text):
    """Reject ambiguous API pairing; preserve raw stacks for ownership analysis."""
    lines = text.splitlines()
    require("+0x068 LastErrorValue : Uint4B" in text and "Type referenced:" not in text,
            "Matching local ntdll type information is required for last-error interpretation")
    pending, events = {}, []
    entry = re.compile(r"USER_ENTRY (\w+) tid=(\w+) rsp=(\w+) a0=(\w+) a1=(\w+) a2=(\w+) a3=(\w+) caller=(\w+)$")
    returned = re.compile(r"USER_RETURN (\w+) tid=(\w+) rsp=(\w+) result=(\w+) caller=(\w+)$")
    for index, line in enumerate(lines):
        if line.startswith("USER_ENTRY ") and "*** WARNING:" in line:
            require("Unable to verify checksum" in line and index + 1 < len(lines), "Unrecognized interrupted debugger row")
            line = line.split("*** WARNING:", 1)[0] + lines[index + 1]
        match = entry.fullmatch(line)
        if match:
            api = match[1]
            tid, rsp, *args, caller = (int(value, 16) for value in match.groups()[1:])
            key = (api, tid, rsp + 8, caller)
            require(key not in pending, "Duplicate/interrupted API entry; trace is ambiguous")
            pending[key] = {"api": api, "thread": tid, "args": args, "caller": caller, "entry_line": index + 1}
        match = returned.fullmatch(line)
        if match:
            api = match[1]
            tid, rsp, result, caller = (int(value, 16) for value in match.groups()[1:])
            key = (api, tid, rsp, caller)
            require(key in pending, "API return has no matching entry")
            event = pending.pop(key)
            event.update(result=result, return_line=index + 1)
            if api == "UnhookWindowsHookEx" and not result:
                errors = [re.fullmatch(r"LastErrorValue: \(Win32\) \w+ \((\d+)\) .*", row)
                          for row in lines[index + 1:index + 7]]
                values = [int(value[1]) for value in errors if value]
                require(len(values) == 1, "Failed unhook has no reliable last-error observation")
                event["last_error"] = values[0]
            events.append(event)
    require(not pending and events and len(events) == sum(line.startswith("USER_RETURN ") for line in lines),
            "Missing API return observations")
    require(not any(error in text for error in ("Syntax error", "Unable to resolve breakpoint", "Command file execution failed")),
            "Debugger command failure")
    return {"paired_api_calls": events,
            "limitation": "Pairing is mechanical evidence, not a liveness conclusion. Analyze module load intervals, target creation/exit, native counters and owned-hook controls together. Debugger pauses alter timing."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("hook-control", "raw40"), required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/windows-debug")
    parser.add_argument("--validation-manifest", type=Path, default=ROOT / "out/05a-work/validation-tools/summary.json")
    parser.add_argument("--debugger", type=Path)
    parser.add_argument("--symbols", type=Path, help="Existing local Microsoft symbol cache; no downloads")
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    if sys.platform != "win32" or bool(args.debugger) != bool(args.symbols):
        parser.error("Native Windows required; supply --debugger and --symbols together")
    output = args.output.resolve()
    if not output.is_relative_to((ROOT / "out").resolve()) or output == (ROOT / "out").resolve():
        parser.error("Choose a fresh directory under repository out/")
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Existing evidence is preserved: {error}")
    report = {"schema_version": 1, "status": "FAIL", "qualification": "NOT_RUN",
              "mode": args.mode, "phases": [], "errors": [], "input_sha256": {}}

    def save():
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    def phase(name, command, timeout, env=None):
        result = run_process(command, cwd=ROOT, timeout=timeout, env=env)
        (output / f"{name}.stdout.log").write_text(result.stdout, encoding="utf-8")
        (output / f"{name}.stderr.log").write_text(result.stderr, encoding="utf-8")
        report["phases"].append({"name": name, **asdict(result)})
        save()
        require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors, f"{name} failed")
        return result

    try:
        compiler = shutil.which("cl")
        require(compiler, "Use the repository x64 MSVC developer wrapper")
        source = Path(__file__).with_suffix(".cpp")
        inputs = [Path(__file__).resolve(), source, Path(compiler),
                  *[ROOT / "tools" / name for name in ("process_runner.py", "windows_unelevated.py", "graphics_probe.py")]]
        executable = output / "IntelUserDiagnostic.exe"
        command = [compiler, "/nologo", "/std:c++20", "/EHsc", "/MDd", "/W4", "/WX", "/utf-8"]
        libraries, env, raw = ["user32.lib"], None, None
        if args.mode == "raw40":
            raw_source = Path(__file__).with_name("05c-win32-vulkan.cpp")
            raw_parser = raw_source.with_suffix(".py")
            text = raw_source.read_text(encoding="utf-8")
            replacements = {'eight Win32/Vulkan': 'forty Win32/Vulkan',
                            '\\"lifetimes\\":8': '\\"lifetimes\\":40',
                            'cycle <= 8': 'cycle <= 40', 'sample(8, "final")': 'sample(40, "final")'}
            for old, new in replacements.items():
                require(text.count(old) == 1, "Raw source changed; review fixed40 transformation")
                text = text.replace(old, new)
            source = output / "raw40.cpp"
            source.write_text(text, encoding="utf-8", newline="\n")
            build = args.build_dir.resolve()
            loader, library = build / "bin/vulkan-1.dll", build / "_deps/vulkanloader-build/loader/vulkan-1.lib"
            headers, manifest = build / "_deps/vulkanheaders-src/include", ROOT / "engine/client/windows.manifest"
            inputs += [raw_source, raw_parser, source, loader, library, manifest,
                       ROOT / "dependencies.lock.json", args.validation_manifest.resolve(),
                       *[path for path in headers.rglob("*.h") if path.is_file()]]
            command += ["/I" + str(headers)]
            libraries = [str(library), "user32.lib", "gdi32.lib", "/MANIFEST:EMBED", "/MANIFESTINPUT:" + str(manifest)]
            shutil.copy2(loader, output / "vulkan-1.dll")
            inputs.append(output / "vulkan-1.dll")
            validation = validation_tools(args.validation_manifest.resolve(), digest(ROOT / "dependencies.lock.json"))
            report["validation_tools"] = validation
            (output / "implicit").mkdir()
            settings = output / "vk_layer_settings.txt"
            settings.write_text("# Owned empty settings; required validation is enabled by the fixture.\n", encoding="utf-8")
            inputs.append(settings)
            env, report["layer_environment"] = layer_environment(validation, settings, output / "implicit")
            require(not report["layer_environment"]["driver_selection"], "Ambient driver selection override is unsupported")
            spec = importlib.util.spec_from_file_location("raw_vulkan_diagnostic", raw_parser)
            raw = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(raw)
            raw.LIFETIMES = 40
        command += [str(source), "/Fo" + str(output / "diagnostic.obj"), "/Fe" + str(executable),
                    "/link", "/MACHINE:X64", *libraries]
        report["input_sha256"] = {str(path): digest(path) for path in inputs}
        phase("compile", command, 120)
        report["input_sha256"][str(executable)] = digest(executable)
        if args.mode == "hook-control":
            phase("help", [str(executable), "--help"], 10)
        if args.build_only:
            report["status"] = "BUILT_ONLY"
        else:
            arguments = [str(executable), INTEL if args.mode == "raw40" else "--run"]
            if args.debugger:
                debugger, symbols = args.debugger.resolve(), args.symbols.resolve()
                require(debugger.name.lower() == "cdb.exe" and debugger.is_file() and symbols.is_dir(), "CDB or symbol cache unavailable")
                initial = debugger_commands(output)
                debug_inputs = [debugger, *initial.parent.glob("*.txt"),
                                *[path for path in symbols.rglob("*.pdb") if path.is_file()]]
                report["input_sha256"].update({str(path): digest(path) for path in debug_inputs})
                arguments = [str(debugger), "-G", "-y", "srv*" + str(symbols), "-cf", str(initial), *arguments]
            deadline = 120 if args.mode == "raw40" else 30
            token = output / "native-token.json"
            result = phase("native", [sys.executable, "tools/windows_unelevated.py", "--timeout", str(deadline),
                                      "--evidence", str(token), "--", *arguments], deadline + 25, env)
            evidence = json.loads(token.read_text(encoding="utf-8"))
            require(not result.stderr and evidence["status"] == "PASS"
                    and evidence["security"]["same_user"] and evidence["security"]["same_session"]
                    and not evidence["security"]["child"]["elevated"]
                    and evidence["security"]["child"]["integrity_level"] == "medium"
                    and evidence["result"]["returncode"] == 0 and not evidence["result"]["timed_out"]
                    and not evidence["result"]["cleanup_errors"]
                    and evidence["result"]["stdout"] == result.stdout
                    and evidence["result"]["stderr"] == result.stderr, "Standard-user execution evidence mismatch")
            text = result.stdout
            if args.debugger:
                report["debugger_observations"] = inspect_debugger(text)
                text = "\n".join(line for line in text.splitlines() if line.startswith("{")) + "\n"
            if raw:
                report["rows"] = raw.inspect_trace(text, INTEL)
                report["rejected_mutations"] = raw.parser_mutations(report["rows"], INTEL)
            else:
                report["rows"] = inspect_control(text)
            report["status"] = "DIAGNOSTIC_ONLY"
        require(all(digest(Path(path)) == value for path, value in report["input_sha256"].items()), "Input identities changed")
        if raw:
            require(validation_tools(args.validation_manifest.resolve(), digest(ROOT / "dependencies.lock.json"))
                    == report["validation_tools"], "Validation tools changed during the diagnostic")
        report["identities_unchanged"] = True
    except (OSError, ValueError, KeyError, TypeError, Unavailable) as error:
        report["status"] = "FAIL"
        report["errors"].append(str(error))
    save()
    print(json.dumps({"status": report["status"], "qualification": "NOT_RUN", "summary": str(output / "summary.json")}))
    return 1 if report["status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
