"""Prepare CPU scene fixtures or qualify actual Runtime scene presentation on one GPU."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from functools import lru_cache
import json
import math
from pathlib import Path
import re
import struct
import sys
import tempfile
import time

import offscreen_smoke as raster
import scene_render_smoke
import shader_pipeline
from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect

CASES = ("featureworld", "fresh", "imported")
# Keep the requested client area above the Windows decorated-window minimum.
WIDTH, HEIGHT = 160, 120
TIMEOUT_SECONDS = 30
require = raster.require
integer = raster.integer


def camera(case: str) -> str:
    require(case in CASES, "Unknown Runtime scene case")
    return ("20000000" if case == "featureworld" else "13e00000") + "-0000-4000-8000-000000000004"


@lru_cache(maxsize=3)
def reference(case: str) -> raster.Reference:
    """Fixed source geometry and camera, independent of engine matrices/reports."""
    require(case in CASES, "Unknown Runtime scene case")
    # Orthographic camera, near=1/far=9. Cube front faces are half a unit
    # nearer than their centers; the plane rotates +90 degrees about X.
    if case == "featureworld":
        height = 3.
        shapes = ((((-.5, -.5), (.5, -.5), (.5, .5), (-.5, .5)), .1875),)
    elif case == "fresh":
        height = 4.
        shapes = ((((-1.5, -.5), (-.5, -.5), (-.5, .5), (-1.5, .5)), .3125),
                  (((.25, -.75), (1.75, -.75), (1.75, .75), (.25, .75)), .375))
    else:
        height = 3.
        shapes = ((((-1., -1.), (1., -1.), (-1., 1.)), .25),)
    count = WIDTH * HEIGHT
    color = bytearray((0, 0, 0, 255) * count)
    depth = [1.] * count
    checked = [True] * count
    for shape, z in shapes:
        vertices = [raster.snap_vertex(((x / (height * WIDTH / HEIGHT) + .5) * WIDTH,
                                        (.5 - y / height) * HEIGHT, z, 1.)) for x, y in shape]
        triangles = ((0, 1, 2), (0, 2, 3)) if len(shape) == 4 else ((0, 1, 2),)
        for triangle in triangles:
            a, b, c = (vertices[index] for index in triangle)
            area = raster.edge(a, b, c)
            lengths = [math.hypot(q[0] - p[0], q[1] - p[1])
                       for p, q in ((b, c), (c, a), (a, b))]
            for y in range(HEIGHT):
                for x in range(WIDTH):
                    pixel = y * WIDTH + x
                    weights = [raster.edge(p, q, (x + .5, y + .5)) / area
                               for p, q in ((b, c), (c, a), (a, b))]
                    distances = [weight * abs(area) / length
                                 for weight, length in zip(weights, lengths, strict=True)]
                    if min(distances) >= -1 and min(abs(value) for value in distances) <= 1:
                        checked[pixel] = False
                    if min(weights) >= 0 and z <= depth[pixel]:
                        depth[pixel] = z
                        color[pixel * 4:pixel * 4 + 4] = bytes((255,) * 4)
    foreground = sum(ok and value < 1 for ok, value in zip(checked, depth, strict=True))
    background = sum(ok and value == 1 for ok, value in zip(checked, depth, strict=True))
    require(foreground + background >= count * .8 and foreground >= count * .03,
            "Reference lacks checked foreground/background")
    return raster.Reference(WIDTH, HEIGHT, bytes(color), tuple(depth), tuple(checked),
                            foreground, background, 0)


def read_json(path: Path) -> dict:
    require(path.stat().st_size <= 1024 * 1024, "Runtime JSON exceeds limit")
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object,
                       parse_constant=reject_constant, parse_float=finite_float)
    require(isinstance(value, dict), "Runtime JSON must be an object")
    return value


def content_inputs(root: Path) -> dict[str, str]:
    names = [f"{case}/scene.bin" for case in CASES] + ["imported/mesh.bin"]
    limits = {"scene.bin": 4 * 1024 * 1024, "mesh.bin": 2 * 1024 * 1024}
    for name in names:
        path = root / name
        require(path.is_file() and not path.is_symlink() and
                44 <= path.stat().st_size <= limits[path.name], "Missing/unbounded prepared input")
        require(path.read_bytes()[:8] == (b"SHKSCNE1" if path.name == "scene.bin" else b"SHKMESH1"),
                "Fixture is not prepared content")
    return {name: sha256(root / name) for name in names}


def prepare(executable: Path, output: Path) -> ProcessResult:
    result = run_process([str(executable), "--source-root", str(ROOT), "--output", str(output)],
                         cwd=executable.parent, timeout=TIMEOUT_SECONDS)
    require(result.returncode == 0 and not result.stderr and not result.timed_out and
            not result.cleanup_errors, "CPU Runtime scene preparation failed: " + result.stderr)
    require(json.loads(result.stdout) == {"status": "PASS", "cases": 3, "device_accessed": False},
            "Preparation did not complete all cases")
    content_inputs(output)
    for case in CASES:
        require((output / case / "project.sharkproject").is_file(), "Missing saved source project")
    require((output / "imported/LICENSE").read_bytes() ==
            (ROOT / "tests/fixtures/gltf_source/LICENSE").read_bytes(), "Lost imported fixture license")
    return result


def inspect_capture(case: str, output: Path, device: str, result: ProcessResult) -> tuple[dict, dict]:
    require(result.returncode == 0 and not result.stderr and not result.timed_out and
            not result.cleanup_errors, "Runtime failed, timed out or emitted diagnostics")
    report = read_json(output / "report.json")
    require(json.loads(result.stdout, object_pairs_hook=unique_object,
                       parse_constant=reject_constant, parse_float=finite_float) == report,
            "Runtime stdout and saved completion differ")
    require(report.get("event") == "runtime-scene-complete" and
            report.get("adapter_uuid") == device and report.get("validation") == "required",
            "Runtime did not use selected validated GPU")
    require(report.get("width") == WIDTH and report.get("height") == HEIGHT and
            report.get("rendered_frames") == 2, "Runtime did not present requested frames/extent")
    require(report.get("draw_count") == (2 if case == "fresh" else 1) and
            report.get("mesh_instances") == (2 if case == "fresh" else 1) and
            report.get("hidden") == report.get("culled") == 0, "Unexpected extraction/draw counts")
    require(all(integer(report.get(key)) and report[key] == 0
                for key in ("warnings", "errors", "dropped")), "Graphics diagnostics are not clean")
    require(all(integer(report.get(key)) and report[key] > 0
                for key in ("scene_instance", "scene_revision")), "Missing live scene identity")
    require(all(isinstance(report.get(key), str) and report[key]
                for key in ("adapter_name", "driver_name", "driver_info")) and
            integer(report.get("driver_version")), "Missing actual device/driver")
    state = report.get("presentation", {})
    require(state.get("closed") is True and state.get("faulted") is False and
            state.get("in_flight") == 0 and state.get("completed") == state.get("submitted") and
            integer(state.get("presented")) and state["presented"] >= 2 and
            integer(state.get("generations")) and state["generations"] >= 1 and
            state.get("retired_generations") == state["generations"], "Presentation owners did not retire")
    require(report.get("capture") == {"color": "color.rgba", "depth": "depth.f64", "color_space": "srgb",
                                      "subpixel_precision_bits": 8},
            "Capture format changed")
    require({path.name for path in output.iterdir()} == {"report.json", "color.rgba", "depth.f64"},
            "Capture has unexpected files")
    require((output / "color.rgba").stat().st_size == WIDTH * HEIGHT * 4 and
            (output / "depth.f64").stat().st_size == WIDTH * HEIGHT * 8, "Wrong capture extent/format")
    raw = (output / "color.rgba").read_bytes()
    depths = struct.unpack(f"<{WIDTH * HEIGHT}d", (output / "depth.f64").read_bytes())
    require(all(math.isfinite(value) and 0 <= value <= 1 for value in depths), "Invalid capture depth")
    comparison = raster.compare_readback(raw, struct.pack(f"<{len(depths)}f", *depths), reference(case))
    # All scene factors are white; sRGB encoding preserves these independent
    # endpoints exactly. General sRGB conversion is covered by the cube fixture.
    comparison.update(case=case, color_sha256=sha256(output / "color.rgba"),
                      depth_sha256=sha256(output / "depth.f64"))
    return report, comparison


def inspect_lifecycle(result: ProcessResult, device: str, baseline: dict,
                      baseline_depth: bytes) -> dict:
    require(result.returncode == 0 and not result.stderr and not result.timed_out and
            not result.cleanup_errors, "Runtime lifecycle failed, timed out or emitted diagnostics")
    require(len(result.stdout.encode("utf-8")) <= 1024 * 1024, "Lifecycle JSON exceeds limit")
    row = json.loads(result.stdout, object_pairs_hook=unique_object,
                     parse_constant=reject_constant, parse_float=finite_float)
    require(isinstance(row, dict) and row.get("event") == "runtime-scene-lifecycle-complete" and
            row.get("status") == "PASS" and row.get("adapter_uuid") == device and
            row.get("validation") == "required", "Lifecycle omitted validated completion")
    require(all(row.get(key) == baseline.get(key) for key in
                ("adapter_name", "driver_name", "driver_info", "driver_version")),
            "Lifecycle device/driver differs from Runtime capture")
    require(row.get("replacement_rejection") == {"fault": "window_draw_partial_creation", "fired": True,
                                                  "old_frame_preserved": True},
            "Lifecycle omitted failed replacement and last-valid frame proof")
    require(integer(row.get("checks")) and row["checks"] >= 30 and
            row.get("closed") is True and row.get("in_flight") == 0 and
            integer(row.get("submitted")) and row["submitted"] >= 7 and
            row.get("completed") == row["submitted"] and row.get("presented") == 7 and
            integer(row.get("generations")) and row["generations"] >= 3 and
            row.get("retired_generations") == row["generations"] and
            all(integer(row.get(key)) and row[key] == 0 for key in ("warnings", "errors", "dropped")),
            "Lifecycle checks/completion/retirement/diagnostics differ")
    occupied = sum(value < 1 for value in struct.unpack(f"<{WIDTH * HEIGHT}d", baseline_depth))
    phases = row.get("phases")
    names = ("baseline", "replacement-rejected", "destroyed", "reloaded", "resized",
             "restored", "retained-snapshot")
    require(isinstance(phases, list) and len(phases) == len(names), "Lifecycle phase count changed")
    for phase, name in zip(phases, names, strict=True):
        resized = name == "resized"
        empty = name == "destroyed"
        require(isinstance(phase, dict) and set(phase) ==
                {"phase", "width", "height", "draws", "foreground_pixels"} and
                phase["phase"] == name and phase["width"] == (200 if resized else WIDTH) and
                phase["height"] == (144 if resized else HEIGHT) and phase["draws"] == (0 if empty else 1),
                "Lifecycle phase order, extent or draw count changed")
        foreground = phase["foreground_pixels"]
        require(integer(foreground) and
                (foreground > occupied if resized else foreground == (0 if empty else occupied)),
                "Lifecycle lost/deviated from its known visible mesh")
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--prepare-executable", type=Path)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--lifecycle", type=Path)
    parser.add_argument("--prepared", type=Path)
    parser.add_argument("--device")
    parser.add_argument("--shaders", type=Path)
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.prepare_only:
        if not args.prepare_executable or any((args.runtime, args.lifecycle, args.prepared, args.device, args.shaders,
                                               args.validation_manifest, args.output)):
            parser.error("--prepare-only needs only --prepare-executable")
        with tempfile.TemporaryDirectory(prefix="shark-runtime-scenes-") as scratch:
            result = prepare(args.prepare_executable.resolve(), Path(scratch) / "fixtures")
            print(result.stdout, end="")
        return 0
    if (sys.platform != "win32" or not all((args.runtime, args.lifecycle, args.prepared, args.shaders,
                                            args.validation_manifest, args.output)) or
            not re.fullmatch(r"[0-9a-f]{32}", args.device or "")):
        parser.error("Native mode needs Windows, Runtime/lifecycle, prepared inputs, shaders, validation, UUID and fresh output")
    output = args.output.absolute()
    shader_pipeline.no_links(output)
    output.mkdir(parents=True, exist_ok=False)
    summary = {"schema_version": 1, "checkpoint": "13e", "status": "FAIL", "qualification": "NOT_RUN",
               "started_utc": datetime.now(timezone.utc).isoformat(), "runs": [], "errors": []}
    started = time.monotonic()
    exit_code = 1
    try:
        executable, lifecycle, prepared = args.runtime.resolve(), args.lifecycle.resolve(), args.prepared.resolve()
        inputs = content_inputs(prepared)
        manifest, vertex, pixel = scene_render_smoke.shader_inputs(args.shaders.resolve())
        lock = sha256(ROOT / "dependencies.lock.json")
        validation = validation_tools(args.validation_manifest.resolve(), lock)
        summary.update(runtime_sha256=sha256(executable), lifecycle_sha256=sha256(lifecycle), prepared_inputs=inputs,
                       shader_manifest=manifest, dependency_lock_sha256=lock,
                       validation_tools=validation, process_security=check_layer_process())
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Required core/synchronization and NVRHI validation.\n", encoding="utf-8")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, summary["layer_environment"] = layer_environment(validation, settings, implicit)
        (output / "environment.json").write_text(json.dumps(collect(args.build_dir), indent=2) + "\n",
                                                 encoding="utf-8")
        for case in CASES:
            capture = output / case
            command = [str(executable), "--scene", str(prepared / case / "scene.bin"),
                       "--camera", camera(case), "--vertex-shader", str(vertex), "--pixel-shader", str(pixel),
                       "--extent", f"{WIDTH}x{HEIGHT}", "--frames", "2", "--device", args.device,
                       "--validation", "required", "--capture-directory", str(capture)]
            if case == "imported":
                command += ["--mesh", str(prepared / case / "mesh.bin")]
            result = run_process(command, cwd=output, timeout=TIMEOUT_SECONDS, env=environment)
            (output / f"{case}.stdout.log").write_text(result.stdout, encoding="utf-8")
            (output / f"{case}.stderr.log").write_text(result.stderr, encoding="utf-8")
            run = {"case": case, "command": command, "exit_code": result.returncode,
                   "timed_out": result.timed_out, "cleanup_errors": list(result.cleanup_errors), "errors": []}
            summary["runs"].append(run)
            try:
                report, comparison = inspect_capture(case, capture, args.device, result)
                run.update(report=report, comparison=comparison)
                raster.write_png(output / f"{case}.png", WIDTH, HEIGHT, (capture / "color.rgba").read_bytes())
                raster.write_png(output / f"{case}-reference.png", WIDTH, HEIGHT, reference(case).color)
                require(comparison["status"] == "PASS", case + " failed fixed color/depth tolerances")
            except (OSError, ValueError, KeyError, TypeError) as error:
                run["errors"].append(str(error))
                summary["errors"].append(case + ": " + str(error))
        command = [str(lifecycle), "--scene", str(prepared / "featureworld/scene.bin"),
                   "--camera", camera("featureworld"), "--vertex-shader", str(vertex), "--pixel-shader", str(pixel),
                   "--device", args.device, "--validation", "required"]
        result = run_process(command, cwd=output, timeout=TIMEOUT_SECONDS, env=environment)
        (output / "lifecycle.stdout.log").write_text(result.stdout, encoding="utf-8")
        (output / "lifecycle.stderr.log").write_text(result.stderr, encoding="utf-8")
        run = {"case": "lifecycle", "command": command, "exit_code": result.returncode,
               "timed_out": result.timed_out, "cleanup_errors": list(result.cleanup_errors), "errors": []}
        summary["runs"].append(run)
        try:
            run["report"] = inspect_lifecycle(result, args.device,
                                               read_json(output / "featureworld/report.json"),
                                               (output / "featureworld/depth.f64").read_bytes())
        except (OSError, ValueError, KeyError, TypeError) as error:
            run["errors"].append(str(error))
            summary["errors"].append("lifecycle: " + str(error))
        require(content_inputs(prepared) == inputs and sha256(executable) == summary["runtime_sha256"] and
                sha256(lifecycle) == summary["lifecycle_sha256"],
                "Runtime or prepared inputs changed during qualification")
        require(scene_render_smoke.shader_inputs(args.shaders.resolve())[0] == manifest and
                validation_tools(args.validation_manifest.resolve(), lock) == validation,
                "Shader/validation inputs changed during qualification")
        if not summary["errors"]:
            summary.update(status="PASS", qualification="PASS")
            exit_code = 0
    except Unavailable as error:
        summary.update(status="NOT_RUN", errors=[str(error)])
        exit_code = 3
    except (OSError, ValueError, TypeError, KeyError) as error:
        summary["errors"].append(str(error))
    finally:
        summary.update(exit_code=exit_code, elapsed_seconds=round(time.monotonic() - started, 3))
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "summary": str(output / "summary.json")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
