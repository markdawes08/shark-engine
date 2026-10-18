"""Qualify two fixed textured-cube readbacks against an independent CPU rasterizer.

Native execution is explicit and bounded. Validation-off output is diagnostic;
synthetic CPU fixtures exercise the oracle and parser without qualifying a GPU.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
from functools import lru_cache
import json
import math
from pathlib import Path
import re
import struct
import sys
import time
import zlib

import graphics_device_smoke as foundation
import shader_pipeline
from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect, command_output

EXTENTS = ((96, 80), (128, 128))
TRANSFORM = (.56, .252, .12, 0, 0, .56, -.15, 0, .42, -.336, -.16, 0, 0, 0, .5, 1)
TEXTURE = (255, 32, 16, 255, 16, 255, 32, 255, 32, 16, 255, 255, 255, 224, 16, 255)
COLOR_TOLERANCE = 1
DEPTH_TOLERANCE = 1e-5
EDGE_EXCLUSION_PIXELS = 1.0
UV_EXCLUSION = .02
MIN_CHECKED_FRACTION = .55
MIN_FOREGROUND_FRACTION = .10
MIN_OCCLUSION_PIXELS = 50
SUBPIXEL_PRECISION_BITS = 8
TIMEOUT_SECONDS = 45
source_digest = foundation.source_digest


def source_inputs() -> dict[str, str]:
    inputs = foundation.source_inputs()
    del inputs["tests/graphics_device_smoke.cpp"]
    inputs["tests/offscreen_smoke.cpp"] = sha256(ROOT / "tests/offscreen_smoke.cpp")
    return dict(sorted(inputs.items()))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def integer(value: object, maximum: int = 0xFFFFFFFFFFFFFFFF) -> bool:
    return type(value) is int and 0 <= value <= maximum


def fixture(case: int) -> dict:
    """The reviewed workload, independent of all values reported by the GPU child."""
    require(type(case) is int and 0 <= case < len(EXTENTS), "Unknown offscreen case")
    faces = (
        ((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)),
        ((1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)),
        ((1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)),
        ((-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)),
        ((-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)),
        ((-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)),
    )
    uvs = ((0, 1), (1, 1), (1, 0), (0, 0))
    vertices = [[x * .6, y * .6, z * .6, *uv]
                for face in faces for (x, y, z), uv in zip(face, uvs, strict=True)]
    vertices += [[vertex[0] * .65 + .384, vertex[1] * .65 - .48,
                  vertex[2] * .65 - .512, .25, .25] for vertex in vertices.copy()]
    triangles = [[offset + index for index in triangle]
                 for offset in range(0, 48, 4) for triangle in ((0, 1, 2), (0, 2, 3))]
    texture = list(TEXTURE)
    if case == 1:
        triangles.reverse()
        texture[:4], texture[-4:] = texture[-4:], texture[:4]
    width, height = EXTENTS[case]
    name = f"cube-{width}x{height}"
    return {"name": name, "extent": {"width": width, "height": height},
            "vertices": vertices, "indices": [index for triangle in triangles for index in triangle],
            "transform": list(TRANSFORM), "texture_extent": {"width": 2, "height": 2},
            "texture_rgba8": texture, "color_file": name + ".rgba8", "depth_file": name + ".d32f"}


def close_values(actual: object, expected: object) -> bool:
    if isinstance(expected, dict):
        return (isinstance(actual, dict) and set(actual) == set(expected)
                and all(close_values(actual[key], value) for key, value in expected.items()))
    if isinstance(expected, list):
        return (isinstance(actual, list) and len(actual) == len(expected)
                and all(close_values(a, b) for a, b in zip(actual, expected, strict=True)))
    if isinstance(expected, float):
        return type(actual) in (int, float) and math.isfinite(actual) and abs(actual - expected) <= 1e-6
    return type(actual) is type(expected) and actual == expected


@dataclass(frozen=True)
class Reference:
    width: int
    height: int
    color: bytes
    depth: tuple[float, ...]
    checked: tuple[bool, ...]
    foreground: int
    background: int
    occlusion: int


def edge(a: tuple, b: tuple, p: tuple) -> float:
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])


def project(vertex: list, matrix: list, width: int, height: int) -> tuple:
    position = (*vertex[:3], 1)
    clip = [sum(matrix[column * 4 + row] * position[column] for column in range(4))
            for row in range(4)]
    require(clip[3] > 0 and all(math.isfinite(value) for value in clip), "Oracle clip coordinates are invalid")
    x, y, z = (value / clip[3] for value in clip[:3])
    require(-1 <= x <= 1 and -1 <= y <= 1 and 0 <= z <= 1,
            "Fixed oracle workload requires no clipping")
    # Shader inverts projection Y exactly once; Vulkan viewport height is positive.
    return ((x + 1) * width * .5, (1 - y) * height * .5, z, 1 / clip[3], *vertex[3:])


def snap_vertex(vertex: tuple) -> tuple:
    # Vulkan reports framebuffer XY precision through subPixelPrecisionBits:
    # https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-subPixelPrecisionBits
    # Both current-PC devices use nearest 8-bit subpixel positions for this
    # workload. Query and require that property; never fit a grid to readbacks.
    # Interpolation follows the quantized vertices, not their ideal positions.
    scale = 1 << SUBPIXEL_PRECISION_BITS
    return (round(vertex[0] * scale) / scale, round(vertex[1] * scale) / scale, *vertex[2:])


@lru_cache(maxsize=8)
def rasterize(case: int, extent: tuple[int, int] | None = None) -> Reference:
    data = fixture(case)
    width, height = EXTENTS[case] if extent is None else extent
    require(type(width) is int and type(height) is int and 1 <= width <= 1024 and 1 <= height <= 1024,
            "Oracle extent exceeds the bounded resource contract")
    count = width * height
    color = bytearray((0, 0, 0, 255) * count)
    depths = [1.0] * count
    checked = [True] * count
    winning_uv = [None] * count
    layers: list[list[tuple[float, tuple[int, ...]]]] = [[] for _ in range(count)]
    vertices = [snap_vertex(project(vertex, data["transform"], width, height)) for vertex in data["vertices"]]
    for start in range(0, len(data["indices"]), 3):
        a, b, c = (vertices[index] for index in data["indices"][start:start + 3])
        area = edge(a, b, c)
        # CCW front faces after the one Y inversion have negative screen-coordinate area.
        if area >= 0:
            continue
        lengths = [math.hypot(q[0] - p[0], q[1] - p[1]) for p, q in ((b, c), (c, a), (a, b))]
        left = max(0, math.floor(min(a[0], b[0], c[0]) - EDGE_EXCLUSION_PIXELS))
        right = min(width, math.ceil(max(a[0], b[0], c[0]) + EDGE_EXCLUSION_PIXELS))
        top = max(0, math.floor(min(a[1], b[1], c[1]) - EDGE_EXCLUSION_PIXELS))
        bottom = min(height, math.ceil(max(a[1], b[1], c[1]) + EDGE_EXCLUSION_PIXELS))
        for y in range(top, bottom):
            for x in range(left, right):
                pixel = y * width + x
                point = (x + .5, y + .5)
                edges = (edge(b, c, point), edge(c, a, point), edge(a, b, point))
                distances = [-value / length for value, length in zip(edges, lengths, strict=True)]
                if min(distances) >= -EDGE_EXCLUSION_PIXELS and min(abs(value) for value in distances) <= EDGE_EXCLUSION_PIXELS:
                    checked[pixel] = False
                weights = [value / area for value in edges]
                if min(weights) < 0:
                    continue
                depth = sum(weight * vertex[2] for weight, vertex in zip(weights, (a, b, c), strict=True))
                reciprocal_w = sum(weight * vertex[3] for weight, vertex in zip(weights, (a, b, c), strict=True))
                uv = tuple(sum(weight * vertex[3] * vertex[component]
                               for weight, vertex in zip(weights, (a, b, c), strict=True)) / reciprocal_w
                           for component in (4, 5))
                tx, ty = (min(1, max(0, math.floor(value * 2))) for value in uv)
                texel = tuple(data["texture_rgba8"][(ty * 2 + tx) * 4:(ty * 2 + tx + 1) * 4])
                layers[pixel].append((depth, texel))
                if depth <= depths[pixel]:
                    depths[pixel] = depth
                    color[pixel * 4:pixel * 4 + 4] = bytes(texel)
                    winning_uv[pixel] = uv
    for pixel, uv in enumerate(winning_uv):
        if uv is not None and any(abs(value - .5) <= UV_EXCLUSION for value in uv):
            checked[pixel] = False
    foreground = sum(ok and depth < 1 for ok, depth in zip(checked, depths, strict=True))
    background = sum(ok and depth == 1 for ok, depth in zip(checked, depths, strict=True))
    occlusion = sum(checked[pixel] and any(depth > depths[pixel] + 1e-4
                                         and texel != tuple(color[pixel * 4:pixel * 4 + 4])
                                         for depth, texel in samples)
                    for pixel, samples in enumerate(layers))
    require(foreground + background >= math.ceil(count * MIN_CHECKED_FRACTION), "Oracle exclusion exceeds its fixed budget")
    require(foreground >= math.ceil(count * MIN_FOREGROUND_FRACTION), "Oracle foreground coverage is insufficient")
    require(occlusion >= MIN_OCCLUSION_PIXELS, "Oracle lacks independently depth-occluded samples")
    return Reference(width, height, bytes(color), tuple(depths), tuple(checked), foreground, background, occlusion)


def compare_readback(color: bytes, depth_bytes: bytes, reference: Reference) -> dict:
    count = reference.width * reference.height
    require(len(color) == count * 4 and len(depth_bytes) == count * 4, "Readback byte count differs from extent/format")
    depths = struct.unpack(f"<{count}f", depth_bytes)
    require(all(math.isfinite(value) and 0 <= value <= 1 for value in depths), "Depth readback contains nonfinite/out-of-range values")
    max_color, max_depth = 0, 0.0
    bad_color, bad_depth = 0, 0
    examples = []
    for pixel, checked in enumerate(reference.checked):
        if not checked:
            continue
        color_error = max(abs(a - b) for a, b in zip(color[pixel * 4:pixel * 4 + 4],
                                                     reference.color[pixel * 4:pixel * 4 + 4], strict=True))
        depth_error = abs(depths[pixel] - reference.depth[pixel])
        max_color, max_depth = max(max_color, color_error), max(max_depth, depth_error)
        bad_color += color_error > COLOR_TOLERANCE
        bad_depth += depth_error > DEPTH_TOLERANCE
        if len(examples) < 8 and (color_error > COLOR_TOLERANCE or depth_error > DEPTH_TOLERANCE):
            examples.append({"x": pixel % reference.width, "y": pixel // reference.width,
                             "color_error": color_error, "depth_error": depth_error})
    return {"status": "PASS" if bad_color == bad_depth == 0 else "FAIL",
            "checked_pixels": reference.foreground + reference.background,
            "foreground_pixels": reference.foreground, "background_pixels": reference.background,
            "occlusion_pixels": reference.occlusion,
            "excluded_pixels": count - reference.foreground - reference.background,
            "color_tolerance_bytes": COLOR_TOLERANCE, "depth_tolerance": DEPTH_TOLERANCE,
            "max_color_error": max_color, "max_depth_error": max_depth,
            "bad_color_pixels": bad_color, "bad_depth_pixels": bad_depth, "examples": examples}


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    require(len(rgba) == width * height * 4, "PNG input length differs from extent")

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    scanlines = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def inspect_result(result: ProcessResult, mode: str, device: str, validation: str,
                   lock_digest: str, inputs_digest: str, readbacks: Path | None = None) -> tuple[list[dict], list[str], list[dict]]:
    errors = list(result.cleanup_errors)
    if result.timed_out:
        errors.append("Offscreen deadline exceeded; owned process tree terminated")
    if result.returncode != 0:
        errors.append(f"Offscreen fixture exited {result.returncode}")
    if result.stderr.strip():
        errors.append("Offscreen fixture emitted unexpected stderr")
    records, comparisons = [], []
    if len(result.stdout.encode("utf-8")) > 1024 * 1024:
        return [], [*errors, "Offscreen report exceeds its byte budget"], []
    try:
        for line in result.stdout.splitlines():
            row = json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant, parse_float=finite_float)
            require(isinstance(row, dict), "Each offscreen row must be an object")
            records.append(row)
        require(len(records) == (2 if mode == "describe" else 5), "Offscreen report row count differs from workload")
        identity = records[0]
        require(set(identity) == {"kind", "schema_version", "mode", "dependency_lock_sha256", "source_sha256",
                                  "validation", "selection", "assignment_06", "compiler"}, "Identity fields differ from schema")
        for key, expected in (("kind", "identity"), ("schema_version", 1), ("mode", mode),
                              ("dependency_lock_sha256", lock_digest), ("source_sha256", inputs_digest),
                              ("validation", validation), ("selection", "" if mode == "describe" else device),
                              ("assignment_06", "NOT_RUN")):
            require(type(identity.get(key)) is type(expected) and identity[key] == expected,
                    f"Offscreen identity disagrees with inputs: {key}")
        require(isinstance(identity.get("compiler"), str) and bool(identity["compiler"]), "Missing compiler identity")
        if mode == "describe":
            row = records[1]
            expected = {"kind": "description", "status": "PASS", "qualification": "NOT_RUN",
                        "device_accessed": False, "cases": 2}
            require(set(row) == {*expected, "native_graphics_available", "scope"}
                    and all(close_values(row[key], value) for key, value in expected.items())
                    and type(row["native_graphics_available"]) is bool
                    and isinstance(row["scope"], str) and 1 <= len(row["scope"]) <= 256,
                    "Description must be device-free with the fixed case count")
            return records, errors, comparisons
        require(mode == "render" and readbacks is not None, "Render inspection requires its fresh readback directory")
        require(set(records[1]) == {"kind", "adapter"} and records[1]["kind"] == "adapter", "Missing selected adapter row")
        problems = foundation.adapter_errors(records[1]["adapter"], require_supported=True)
        require(not problems, "; ".join(problems))
        require(records[1]["adapter"]["uuid"] == device, "Selected adapter differs from requested UUID")
        expected_files = set()
        previous_submission = 0
        for ordinal, row in enumerate(records[2:4]):
            expected = fixture(ordinal)
            fields = {"kind", "submitted", "completed", "created_resources", "retired_resources",
                      "subpixel_precision_bits", *expected}
            require(set(row) == fields and row.get("kind") == "case", "Offscreen case fields differ from schema")
            for key, value in expected.items():
                require(close_values(row[key], value), f"Case {ordinal + 1} changed fixed input: {key}")
            require(integer(row["submitted"]) and row["submitted"] > previous_submission
                    and integer(row["completed"]) and row["completed"] == row["submitted"],
                    "Offscreen readback lacks a new completed submission")
            previous_submission = row["submitted"]
            require(integer(row["created_resources"]) and row["created_resources"] == 17
                    and integer(row["retired_resources"]) and row["created_resources"] == row["retired_resources"],
                    "Offscreen resources were not retired after completion")
            paths = [shader_pipeline.contained(readbacks, expected[field]) for field in ("color_file", "depth_file")]
            for path in paths:
                expected_files.add(path.name)
                require(path.is_file() and path.stat().st_size == EXTENTS[ordinal][0] * EXTENTS[ordinal][1] * 4,
                        "Readback missing or size differs from fixed extent")
            color, depth = (path.read_bytes() for path in paths)
            bits = row["subpixel_precision_bits"]
            require(integer(bits, 32) and bits >= 4, "Invalid queried subpixel precision")
            if ordinal:
                require(bits == records[2]["subpixel_precision_bits"], "Subpixel precision changed during ownership")
            comparison = {"name": expected["name"], "subpixel_precision_bits": bits,
                          "color_sha256": sha256(paths[0]), "depth_sha256": sha256(paths[1])}
            if bits != SUBPIXEL_PRECISION_BITS:
                comparison.update(status="NOT_RUN", reason="CPU oracle supports the current-PC 8-bit subpixel profile only")
            else:
                comparison.update(compare_readback(color, depth, rasterize(ordinal)))
            comparisons.append(comparison)
            if comparison["status"] == "FAIL":
                errors.append(f"{expected['name']} failed fixed color/depth tolerances")
        require({path.name for path in readbacks.iterdir()} == expected_files, "Readback directory contains unexpected artifacts")
        complete = records[-1]
        require(set(complete) == {"kind", "status", "qualification", "execution_status", "cases", "assignment_06", "diagnostics"},
                "Completion fields differ from schema")
        qualification = "PASS" if validation == "required" else "NOT_RUN"
        for key, expected in (("kind", "complete"), ("status", qualification), ("qualification", qualification),
                              ("execution_status", "PASS"), ("cases", 2), ("assignment_06", "NOT_RUN")):
            require(type(complete.get(key)) is type(expected) and complete[key] == expected, f"Invalid completion field: {key}")
        problems = foundation.diagnostic_errors(complete.get("diagnostics"), require_clean=True)
        require(not problems, "; ".join(problems))
    except (OSError, ValueError, TypeError, KeyError, OverflowError, RecursionError) as error:
        errors.append(f"Invalid offscreen evidence: {error}")
    return records, errors, comparisons


def shader_inputs(root: Path) -> tuple[dict, Path, Path]:
    manifest = shader_pipeline.verify(root)
    require(manifest["inputs"]["recipe"] == shader_pipeline.recipe(ROOT / "shaders/recipe.json"),
            "Shader artifacts do not match the fixed cube recipe")
    require(manifest["inputs"]["toolchain"]["lock_sha256"] == sha256(ROOT / "dependencies.lock.json"),
            "Shader artifacts have a stale dependency lock")
    require(manifest["inputs"]["pipeline_sha256"] == sha256(ROOT / "tools/shader_pipeline.py"),
            "Shader artifacts have a stale builder identity")
    sources = shader_pipeline.source_closure(ROOT / "shaders", manifest["inputs"]["recipe"])
    require(manifest["inputs"]["sources"] == {name: {"sha256": shader_pipeline.digest(data), "bytes": len(data)}
                                               for name, data in sources.items()}, "Shader artifacts have stale source inputs")
    rows = {row["id"]: row for row in manifest["shaders"]}
    require(set(rows) == {"textured-cube-vs", "textured-cube-ps"}, "Expected exactly the reviewed cube shader pair")
    return (manifest, shader_pipeline.contained(root, rows["textured-cube-vs"]["file"]),
            shader_pipeline.contained(root, rows["textured-cube-ps"]["file"]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=("describe", "render"), required=True)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--shaders", type=Path, help="Verified offline shader generation root")
    parser.add_argument("--validation", choices=("required", "off"), default="required")
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.mode == "render" and (not re.fullmatch(r"[0-9a-f]{32}", args.device) or args.shaders is None):
        parser.error("Render requires --device exact lowercase UUID and --shaders artifact root")
    if args.mode == "describe" and (args.device != "auto" or args.validation != "required" or args.shaders is not None):
        parser.error("Description performs no device selection or shader loading")
    executable, output = args.executable.resolve(), args.output.absolute()
    if sys.platform != "win32" or not executable.is_file():
        parser.error("Requires Windows and a built OffscreenSmoke executable")
    try:
        shader_pipeline.no_links(output)
        output.mkdir(parents=True, exist_ok=False)
    except (OSError, ValueError) as error:
        parser.error(f"Choose a fresh writable evidence directory: {error}")
    summary = {"schema_version": 1, "checkpoint": "06b", "status": "FAIL", "qualification": "NOT_RUN",
               "assignment_06": "NOT_RUN", "started_utc": datetime.now(timezone.utc).isoformat(),
               "mode": args.mode, "validation": args.validation, "timeout_seconds": TIMEOUT_SECONDS,
               "scope": "Two fixed offscreen cube readback/upload cases; no window presentation or scene renderer"}
    started, exit_code = time.monotonic(), 1
    try:
        lock_digest, inputs = sha256(ROOT / "dependencies.lock.json"), source_inputs()
        inputs_digest = source_digest(inputs)
        summary.update(dependency_lock_sha256=lock_digest, source_sha256=inputs_digest,
                       input_sha256=inputs, executable_sha256=sha256(executable),
                       git_revision=command_output(["git", "rev-parse", "HEAD"]),
                       git_status=command_output(["git", "status", "--short"]))
        tool_names = ("offscreen_smoke.py", "shader_pipeline.py", "graphics_device_smoke.py", "graphics_probe.py",
                      "process_runner.py", "toolchain_report.py", "windows_unelevated.py")
        summary["tool_inputs"] = {name: sha256(ROOT / "tools" / name) for name in tool_names}
        command = [str(executable), "--" + args.mode]
        manifest = None
        if args.mode == "render":
            manifest, vertex, pixel = shader_inputs(args.shaders.absolute())
            summary["shader_manifest"] = manifest
            command += ["--device", args.device, "--vertex", str(vertex), "--pixel", str(pixel),
                        "--output", str(output / "readbacks"), "--validation", args.validation]
        summary["command"] = command
        validation = None
        if args.mode == "render" and args.validation == "required":
            if args.validation_manifest is None:
                raise Unavailable("Required validation needs --validation-manifest from locked tool preparation")
            validation = validation_tools(args.validation_manifest.resolve(), lock_digest)
            summary["validation_tools"] = {"manifest_path": str(args.validation_manifest.resolve()),
                                           "manifest_sha256": sha256(args.validation_manifest), "receipt": validation}
            summary["process_security"] = check_layer_process()
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Explicit core/synchronization and NVRHI validation; no overrides.\n", encoding="utf-8")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, summary["layer_environment"] = layer_environment(validation, settings, implicit)
        (output / "environment.json").write_text(json.dumps(collect(args.build_dir.resolve() if args.build_dir else None), indent=2) + "\n", encoding="utf-8")
        result = run_process(command, cwd=executable.parent, timeout=TIMEOUT_SECONDS, env=environment)
        (output / "stdout.jsonl").write_text(result.stdout, encoding="utf-8")
        (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
        records, errors, comparisons = inspect_result(result, args.mode, args.device, args.validation,
                                                       lock_digest, inputs_digest, output / "readbacks")
        summary.update(records=records, comparisons=comparisons, errors=errors, process_exit_code=result.returncode,
                       timed_out=result.timed_out, cleanup_errors=list(result.cleanup_errors))
        if validation is not None and validation_tools(args.validation_manifest.resolve(), lock_digest) != validation:
            errors.append("Pinned validation inputs changed during execution")
        if source_inputs() != inputs or sha256(executable) != summary["executable_sha256"]:
            errors.append("Source, lock or executable changed during execution")
        if {name: sha256(ROOT / "tools" / name) for name in tool_names} != summary["tool_inputs"]:
            errors.append("Evidence tools changed during execution")
        if manifest is not None and shader_inputs(args.shaders.absolute())[0] != manifest:
            errors.append("Shader generation changed during execution")
        for index, comparison in enumerate(comparisons):
            data = fixture(index)
            raw = (output / "readbacks" / data["color_file"]).read_bytes()
            write_png(output / (data["name"] + ".png"), *EXTENTS[index], raw)
            if comparison["subpixel_precision_bits"] == SUBPIXEL_PRECISION_BITS:
                reference = rasterize(index)
                write_png(output / (data["name"] + "-reference.png"), *EXTENTS[index], reference.color)
        exit_code = 124 if result.timed_out else (1 if errors else result.returncode)
        if exit_code == 0:
            unsupported = any(comparison["status"] == "NOT_RUN" for comparison in comparisons)
            summary["status"] = "NOT_RUN" if unsupported or (args.mode == "render" and args.validation == "off") else "PASS"
            if unsupported:
                exit_code = 3
            elif args.mode == "render" and args.validation == "required":
                summary["qualification"] = "PASS"
    except Unavailable as error:
        summary.update(status="NOT_RUN", errors=[str(error)])
        exit_code = 3
    except (OSError, ValueError, TypeError, KeyError) as error:
        summary.setdefault("errors", []).append(str(error))
    finally:
        summary.update(exit_code=exit_code, elapsed_seconds=round(time.monotonic() - started, 3),
                       finished_utc=datetime.now(timezone.utc).isoformat())
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "qualification": summary["qualification"],
                      "exit_code": exit_code, "summary": str(output / "summary.json")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
