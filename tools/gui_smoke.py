"""Qualify presented GUI blending, clipping, updates, resize and close cancellation on one GPU."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import struct
import sys
import time

from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from offscreen_smoke import require, write_png
from process_runner import run_process
import shader_pipeline
from toolchain_report import ROOT, collect


PHASES = (("baseline", 160, 120), ("changed", 160, 120),
          ("resized", 200, 144), ("close-cancelled", 200, 144))
VIEWPORT_PHASES = (("scene-baseline", 160, 120), ("scene-camera", 160, 120),
                   ("scene-content", 160, 120), ("scene-target-resized", 160, 120),
                   ("scene-window-resized", 200, 144), ("scene-removed", 200, 144),
                   ("scene-restored", 200, 144), ("close-cancelled", 200, 144))
COLOR_TOLERANCE = 2


def shader_inputs(root: Path, *, scene: bool = False) -> tuple[dict, Path, Path]:
    manifest = shader_pipeline.verify(root)
    recipe = shader_pipeline.recipe(ROOT / ("shaders/scene_recipe.json" if scene else "shaders/gui_recipe.json"))
    require(manifest["inputs"]["recipe"] == recipe and
            manifest["inputs"]["toolchain"]["lock_sha256"] == sha256(ROOT / "dependencies.lock.json") and
            manifest["inputs"]["pipeline_sha256"] == sha256(ROOT / "tools/shader_pipeline.py"),
            "Stale GUI shader recipe/toolchain")
    sources = shader_pipeline.source_closure(ROOT / "shaders", recipe)
    require(manifest["inputs"]["sources"] ==
            {name: {"sha256": shader_pipeline.digest(data), "bytes": len(data)} for name, data in sources.items()},
            "Stale GUI shader sources")
    rows = {row["id"]: row for row in manifest["shaders"]}
    vertex, pixel = ("scene-unlit-vs", "scene-unlit-ps") if scene else ("gui-vs", "gui-ps")
    require(set(rows) == {vertex, pixel}, "Wrong GUI/scene shader pair")
    return (manifest, shader_pipeline.contained(root, rows[vertex]["file"]),
            shader_pipeline.contained(root, rows[pixel]["file"]))


def compare_capture(path: Path, name: str, width: int, height: int, *, viewport: bool = False) -> dict:
    color = path / (name + ".rgba8")
    depth = path / (name + ".d32f")
    require(color.is_file() and not color.is_symlink() and color.stat().st_size == width * height * 4 and
            depth.is_file() and not depth.is_symlink() and depth.stat().st_size == width * height * 4,
            "GUI capture extent/format differs")
    raw = color.read_bytes()
    require(all(value == 1.0 for value in struct.unpack(f"<{width * height}f", depth.read_bytes())),
            "GUI changed clear depth despite disabled depth writes")
    require(all(raw[index] == 255 for index in range(3, len(raw), 4)), "GUI alpha blend lost opaque destination")
    baseline = name == "baseline"
    black, red, blue = (0, 0, 0, 255), (255, 0, 0, 255), (0, 0, 255, 255)
    # Literal interior regions, independent of the native geometry/report. A
    # linear 0.5 channel encodes to sRGB 188. No output-derived baseline/rasterizer.
    regions = (
        ("background", 4, 4, black),
        ("opaque", 28, 28, red if baseline else blue),
        ("old-geometry-edge", 88, 28, red if baseline else black),
        ("overlap", 80, 60, (188, 188, 0, 255) if baseline else (0, 188, 0, 255)),
        ("translucent", 108, 78, (0, 188, 0, 255)),
        ("clip-left", 63, 68, red if baseline else black),
        ("clip-top", 78, 43, red if baseline else black),
        ("clip-right", 123, 68, black),
        ("clip-bottom", 108, 103, black),
    )
    if viewport:
        # Literal screen-space interiors, independently authored from the
        # fixture. Scene image covers x20..140/y20..100. A camera-only update
        # moves the near red half from x20..80 to x50..110; the following content
        # update changes the far blue quad to green, without a revision change.
        removed = name == "scene-removed"
        original = name == "scene-baseline"
        far = blue if name in ("scene-baseline", "scene-camera") else (0, 255, 0, 255)
        under = black if removed else far
        middle = black if removed else red
        overlay = (188, 188, 188, 255) if removed else (
            (188, 188, 255, 255) if far == blue else (188, 255, 188, 255))
        regions = (
            ("background", 4, 4, black),
            ("old-camera-edge", 28, 76, red if original else under),
            ("near-depth", 62, 76, middle),
            ("new-camera-interior", 96, 76, blue if original else middle),
            ("far-content", 118, 76, under),
            ("scene-overlay", 118, 38, overlay),
            ("overlay-clip-left", 84, 38, blue if original else middle),
            ("overlay-clip-top", 118, 24, under),
            ("overlay-clip-right", 134, 38, under),
            ("overlay-clip-bottom", 118, 64, under),
        )
    if width == 200:
        regions += (("resized-background", 180, 130, black),)
    maximum = 0
    for label, left, top, expected in regions:
        for y in range(top, top + 4):
            for x in range(left, left + 4):
                observed = raw[(y * width + x) * 4:(y * width + x + 1) * 4]
                error = max(abs(a - b) for a, b in zip(observed, expected, strict=True))
                maximum = max(maximum, error)
                require(error <= COLOR_TOLERANCE,
                        f"{name}/{label} pixel ({x},{y}) is {list(observed)}, expected {expected}")
    write_png(path.parent / (name + ".png"), width, height, raw)
    return {"name": name, "regions": len(regions), "checked_color_pixels": len(regions) * 16,
            "checked_depth_pixels": width * height, "color_tolerance": COLOR_TOLERANCE,
            "max_color_error": maximum, "color_sha256": sha256(color), "depth_sha256": sha256(depth)}


def inspect(result, captures: Path, device: str, *, viewport: bool = False) -> tuple[dict, list[dict]]:
    require(result.returncode == 0 and not result.stderr and not result.timed_out and not result.cleanup_errors,
            "Native GUI fixture failed, timed out or emitted diagnostics")
    require(len(result.stdout.encode("utf-8")) <= 128 * 1024, "Native GUI report exceeds limit")
    report = json.loads(result.stdout, object_pairs_hook=unique_object,
                        parse_constant=reject_constant, parse_float=finite_float)
    require(isinstance(report, dict) and report.get("schema_version") == 1 and
            report.get("event") == ("gui-viewport-complete" if viewport else "gui-render-complete") and
            report.get("status") == "PASS" and
            report.get("adapter_uuid") == device and report.get("validation") == "required",
            "GUI fixture did not use the explicit validated GPU")
    require(all(isinstance(report.get(key), str) and report[key] for key in
                ("adapter_name", "driver_name", "driver_info")) and type(report.get("driver_version")) is int,
            "Missing actual GUI device/driver identity")
    require(report.get("diagnostics") == {"warnings": 0, "errors": 0, "dropped": 0, "messages": []},
            "GUI graphics validation is not clean")
    phases = report.get("phases")
    expected_phases = VIEWPORT_PHASES if viewport else PHASES
    require(isinstance(phases, list) and len(phases) == len(expected_phases), "GUI fixture omitted a lifecycle phase")
    comparisons, generations, expected_files = [], [], set()
    submitted, presented = 0, 0
    for index, (row, (name, width, height)) in enumerate(zip(phases, expected_phases, strict=True)):
        require(row.get("name") == name and row.get("width") == width and row.get("height") == height and
                row.get("color_file") == name + ".rgba8" and row.get("depth_file") == name + ".d32f",
                "GUI phase order/extent/capture changed")
        state = row["presentation"]
        if viewport:
            require(type(row.get("image_count")) is int and 1 <= row["image_count"] <= 16 and
                    type(row.get("iterations")) is int and row["iterations"] == row["image_count"] * 2 + 1,
                    "GUI viewport phase omitted bounded acquired-slot reuse")
            expected_viewport = None if name == "scene-removed" else {
                "width": 96 if index < 3 else 144, "height": 64 if index < 3 else 96,
                "scene_instance": 600, "revision": 9}
            require(row.get("viewport") == expected_viewport, "GUI viewport identity/extent changed")
            presented += row["iterations"]
        else:
            presented += 1
        require(state["closed"] is False and state["faulted"] is False and state["presented"] == presented and
                type(state["submitted"]) is int and state["submitted"] > submitted and
                type(state["generation"]) is int and state["generation"] >= 1,
                "GUI phase did not advance presentation")
        submitted = state["submitted"]
        generations.append(state["generation"])
        comparisons.append(compare_capture(captures, name, width, height, viewport=viewport))
        expected_files.update((name + ".rgba8", name + ".d32f"))
    if viewport:
        require(len(set(generations[:4])) == 1 and generations[4] > generations[3] and
                len(set(generations[4:])) == 1, "GUI viewport/window generation transitions changed")
        digests = [item["color_sha256"] for item in comparisons]
        require(len(set(digests[:3])) == 3 and digests[4] != digests[5] and
                digests[4] == digests[6] == digests[7],
                "GUI viewport camera/content/removal/restoration transitions were not observed")
    else:
        require(generations[0] == generations[1] and generations[2] > generations[1] and
                generations[2] == generations[3], "GUI resize did not replace the presentation generation")
        require(comparisons[0]["color_sha256"] != comparisons[1]["color_sha256"] and
                comparisons[2]["color_sha256"] == comparisons[3]["color_sha256"],
                "GUI update/close cancellation did not preserve the expected image transitions")
    state = report["presentation"]
    require(state["closed"] is True and state["faulted"] is False and state["in_flight"] == 0 and
            state["presented"] == presented and state["submitted"] == state["completed"] == submitted and
            state["generation"] == state["retired_generations"] == generations[-1],
            "GUI close did not drain submissions and retire all generations")
    require({item.name for item in captures.iterdir()} == expected_files, "Unexpected GUI capture outputs")
    return report, comparisons


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--shaders", type=Path, required=True)
    parser.add_argument("--scene-shaders", type=Path,
                        help="Enable the 16b scene viewport composition phases with this verified shader bundle")
    parser.add_argument("--validation-manifest", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "win32" or not re.fullmatch(r"[0-9a-f]{32}", args.device):
        parser.error("GUI qualification requires Windows and an explicit GPU UUID")
    output = args.output.absolute()
    shader_pipeline.no_links(output)
    output.mkdir(parents=True, exist_ok=False)
    summary = {"schema_version": 1, "checkpoint": "16b" if args.scene_shaders else "16a", "status": "FAIL", "qualification": "NOT_RUN",
               "started_utc": datetime.now(timezone.utc).isoformat(), "errors": []}
    started = time.monotonic()
    code = 1
    try:
        executable = args.executable.resolve()
        manifest, vertex, pixel = shader_inputs(args.shaders.resolve())
        scene_inputs = shader_inputs(args.scene_shaders.resolve(), scene=True) if args.scene_shaders else None
        lock = sha256(ROOT / "dependencies.lock.json")
        validation = validation_tools(args.validation_manifest.resolve(), lock)
        summary.update(executable_sha256=sha256(executable), dependency_lock_sha256=lock,
                       shader_manifest=manifest, validation_tools=validation, process_security=check_layer_process())
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Required core/synchronization and NVRHI validation.\n", encoding="utf-8")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, summary["layer_environment"] = layer_environment(validation, settings, implicit)
        (output / "environment.json").write_text(json.dumps(collect(args.build_dir), indent=2) + "\n", encoding="utf-8")
        captures = output / "captures"
        command = [str(executable), "--device", args.device, "--vertex", str(vertex), "--pixel", str(pixel),
                   "--output", str(captures)]
        if scene_inputs:
            summary["scene_shader_manifest"] = scene_inputs[0]
            command += ["--scene-vertex", str(scene_inputs[1]), "--scene-pixel", str(scene_inputs[2])]
        result = run_process(command, cwd=output, timeout=45, env=environment)
        (output / "native.stdout.log").write_text(result.stdout, encoding="utf-8")
        (output / "native.stderr.log").write_text(result.stderr, encoding="utf-8")
        summary.update(command=command, native_exit_code=result.returncode, timed_out=result.timed_out,
                       cleanup_errors=list(result.cleanup_errors))
        summary["report"], summary["comparisons"] = inspect(result, captures, args.device, viewport=scene_inputs is not None)
        require(sha256(executable) == summary["executable_sha256"] and
                shader_inputs(args.shaders.resolve())[0] == manifest and
                validation_tools(args.validation_manifest.resolve(), lock) == validation,
                "GUI shader/validation/executable inputs changed during qualification")
        if scene_inputs:
            require(shader_inputs(args.scene_shaders.resolve(), scene=True) == scene_inputs,
                    "Scene viewport shader inputs changed during qualification")
        summary.update(status="PASS", qualification="PASS")
        code = 0
    except Unavailable as error:
        summary.update(status="NOT_RUN", errors=[str(error)])
        code = 3
    except (OSError, ValueError, TypeError, KeyError) as error:
        summary["errors"].append(str(error))
    finally:
        summary.update(exit_code=code, elapsed_seconds=round(time.monotonic() - started, 3))
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "summary": str(output / "summary.json")}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
