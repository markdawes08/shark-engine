"""Bounded window/offscreen cube qualification with independent fixed readbacks."""

from __future__ import annotations

import argparse
from dataclasses import replace
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import time

import offscreen_smoke as reference
import graphics_device_smoke as foundation
from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from process_runner import ProcessResult, run_process
from shader_pipeline import contained
from toolchain_report import ROOT, collect, command_output

EXTENTS = ((192, 128), (256, 192), (192, 192))
RESOURCE_FIELDS = {'window_draw_resources', 'window_draw_submissions', 'live_swapchains',
                   'offscreen_resources', 'offscreen_submissions', 'offscreen_recordings', 'pending_queue_workers'}
require = reference.require
integer = reference.integer


def source_inputs() -> dict[str, str]:
    inputs = foundation.source_inputs()
    del inputs['tests/graphics_device_smoke.cpp']
    inputs['tests/cube_smoke.cpp'] = sha256(ROOT / 'tests/cube_smoke.cpp')
    return dict(sorted(inputs.items()))


def srgb_reference(extent: tuple[int, int]) -> reference.Reference:
    source = reference.rasterize(0, extent)
    encoded = bytearray(source.color)
    for i, value in enumerate(encoded):
        if i % 4 != 3:
            linear = value / 255
            encoded[i] = round(255 * (12.92 * linear if linear <= 0.0031308 else 1.055 * linear ** (1 / 2.4) - 0.055))
    return replace(source, color=bytes(encoded))


def stats(row: object, count: int, closed: bool = False) -> None:
    fields = {'submitted', 'completed', 'presented', 'generation', 'retired_generations',
              'suspended_frames', 'in_flight', 'frame_limit', 'active', 'closed', 'faulted'}
    require(isinstance(row, dict) and set(row) == fields, 'Invalid presentation stats schema')
    require(all(integer(row[k]) for k in fields - {'active', 'closed', 'faulted'}), 'Stats counters must be integers')
    require(row['submitted'] == row['presented'] == count and row['completed'] <= count
            and row['frame_limit'] in (1, 2) and row['in_flight'] <= row['frame_limit']
            and row['in_flight'] == count - row['completed'] and 1 <= row['generation'] <= 12
            and row['retired_generations'] == row['generation'] - (0 if closed else 1),
            'Invalid presentation lifecycle counters')
    require(row['closed'] is closed and row['faulted'] is False and row['active'] is (not closed),
            'Unexpected lifecycle state')
    if closed:
        require(row['completed'] == count and row['in_flight'] == 0
                and row['retired_generations'] == row['generation'], 'Closed resources have not retired')


def resources(row: object, closed: bool = False) -> None:
    require(isinstance(row, dict) and set(row) == RESOURCE_FIELDS and all(integer(v, 4096) for v in row.values()),
            'Invalid resource schema')
    if closed:
        require(all(v == 0 for v in row.values()), 'Native resources remain after close')
    else:
        require(row['live_swapchains'] == 1 and 1 <= row['window_draw_resources'] <= 512
                and row['window_draw_submissions'] <= 8 and row['offscreen_resources'] == row['offscreen_submissions']
                == row['offscreen_recordings'] == row['pending_queue_workers'] == 0,
                'Resource ownership exceeds bounded current lifecycle')


def inspect_result(result: ProcessResult, mode: str, device: str, validation: str, lock: str,
                   digest: str, bundle_hash: str, output: Path) -> tuple[list[dict], list[str], list[dict]]:
    errors = list(result.cleanup_errors)
    rows = []
    comparisons = []
    if result.timed_out or result.returncode or result.stderr.strip():
        errors.append('Child failed, timed out or emitted unexpected stderr')
    try:
        require(mode in {'describe', 'smoke'} and validation in {'required', 'off'}, 'Unknown cube verification mode')
        require(len(result.stdout.encode('utf-8')) <= 1024 * 1024, 'Cube output exceeds byte limit')
        rows = [json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant, parse_float=finite_float)
                for line in result.stdout.splitlines()]
        require(all(isinstance(r, dict) for r in rows), 'Expected JSON objects')
        kinds = (['identity', 'description'] if mode == 'describe'
                 else ['identity', 'adapter', 'case', 'minimized', 'case', 'case', 'complete'])
        require([r.get('kind') for r in rows] == kinds, 'Incomplete or unordered cube workload')
        identity = rows[0]
        expected = {'kind': 'identity', 'schema_version': 1, 'mode': mode,
                    'selection': '' if mode == 'describe' else device, 'validation': validation,
                    'dependency_lock_sha256': lock, 'source_sha256': digest}
        require(set(identity) == {*expected, 'compiler'}
                and all(type(identity[k]) is type(v) and identity[k] == v for k, v in expected.items())
                and isinstance(identity['compiler'], str) and bool(identity['compiler']), 'Identity differs from current inputs')
        if mode == 'describe':
            description = {'kind': 'description', 'device_accessed': False, 'cases': 3, 'qualification': 'NOT_RUN'}
            require(set(rows[1]) == set(description)
                    and all(type(rows[1][key]) is type(value) and rows[1][key] == value
                            for key, value in description.items()), 'Description must be device-free')
            return (rows, errors, comparisons)
        require(set(rows[1]) == {'kind', 'adapter', 'bundle_sha256'} and rows[1]['bundle_sha256'] == bundle_hash,
                'Bundle identity mismatch')
        require(not foundation.adapter_errors(rows[1]['adapter'], require_supported=True)
                and rows[1]['adapter']['uuid'] == device, 'Unexpected or unsupported adapter')
        expected_files = set()
        previous = 0
        previous_generation = 0
        for index, row in enumerate([rows[2], rows[4], rows[5]]):
            require(set(row) == {'kind', 'index', 'window', 'offscreen', 'stats', 'resources'}
                    and type(row['index']) is int and row['index'] == index, 'Case identity mismatch')
            stats(row['stats'], (index + 1) * 3)
            require(row['stats']['generation'] > previous_generation, 'Resize did not advance the swapchain generation')
            previous_generation = row['stats']['generation']
            resources(row['resources'])
            for target in ('window', 'offscreen'):
                readback = row[target]
                w, h = EXTENTS[index]
                name = f'{target}-{index}'
                fields = {'extent', 'submitted', 'completed', 'subpixel_precision_bits', 'color_encoding',
                          'created_resources', 'retired_resources', 'color_file', 'depth_file'}
                require(isinstance(readback, dict) and set(readback) == fields, 'Readback schema mismatch')
                require(readback['extent'] == {'width': w, 'height': h}
                        and all(type(v) is int for v in readback['extent'].values()), 'Readback extent mismatch')
                require(integer(readback['submitted']) and readback['submitted'] > previous
                        and integer(readback['completed']) and readback['completed'] == readback['submitted'],
                        'Readback lacks current completion')
                previous = readback['submitted']
                require(type(readback['subpixel_precision_bits']) is int and readback['subpixel_precision_bits'] == 8,
                        'Oracle requires current-PC subpixel precision')
                require(readback['color_encoding'] == ('srgb' if target == 'window' else 'linear'), 'Incorrect color encoding')
                require(integer(readback['created_resources'], 64) and integer(readback['retired_resources'], 64),
                        'Invalid owner counts')
                require(readback['retired_resources'] == (17 if target == 'offscreen' else 0),
                        'Readback resources retired at wrong lifetime boundary')
                require(readback['created_resources'] == (17 if target == 'offscreen' else 15),
                        'Readback resource count mismatch')
                paths = []
                for field, suffix in [('color_file', '.rgba8'), ('depth_file', '.d32f')]:
                    require(readback[field] == name + suffix, 'Readback filename mismatch')
                    path = contained(output, name + suffix)
                    expected_files.add(path.name)
                    require(path.is_file() and path.stat().st_size == w * h * 4, 'Missing or incorrectly sized readback')
                    paths.append(path)
                ref = srgb_reference((w, h)) if target == 'window' else reference.rasterize(0, (w, h))
                comparison = {'name': name, **reference.compare_readback(paths[0].read_bytes(), paths[1].read_bytes(), ref),
                              'color_sha256': sha256(paths[0]), 'depth_sha256': sha256(paths[1])}
                comparisons.append(comparison)
                if comparison['status'] != 'PASS':
                    errors.append(name + ' failed fixed color/depth tolerance')
        require({p.name for p in output.iterdir()} == expected_files, 'Unexpected readback artifact')
        minimized = rows[3]
        require(set(minimized) == {'kind', 'before', 'after'}, 'Minimize schema mismatch')
        stats(minimized['before'], 3)
        stats(minimized['after'], 3)
        require(minimized['before'] == rows[2]['stats'], 'Minimize did not start from the last recorded state')
        require(minimized['after'] == {**minimized['before'],
                                      'suspended_frames': minimized['before']['suspended_frames'] + 1},
                'Minimize changed GPU ownership or did not suspend exactly one frame')
        final = rows[-1]
        require(set(final) == {'kind', 'status', 'stats', 'resources', 'diagnostics'}
                and final['status'] == ('PASS' if validation == 'required' else 'NOT_RUN'), 'Completion mismatch')
        stats(final['stats'], 9, True)
        resources(final['resources'], True)
        require(final['stats']['generation'] == previous_generation and final['stats']['suspended_frames'] >= 1,
                'Resize/minimize workload incomplete')
        require(not foundation.diagnostic_errors(final['diagnostics'], require_clean=True), 'Unexpected validation diagnostics')
    except (OSError, ValueError, TypeError, KeyError, OverflowError, RecursionError) as error:
        errors.append(str(error))
    return (rows, errors, comparisons)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--mode', choices=['describe', 'smoke'], required=True)
    p.add_argument('--device', default='auto')
    p.add_argument('--bundle', type=Path)
    p.add_argument('--validation', choices=['required', 'off'], default='required')
    p.add_argument('--validation-manifest', type=Path)
    p.add_argument('--build-dir', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    exe = a.executable.resolve()
    out = a.output.absolute()
    if sys.platform != 'win32' or not exe.is_file():
        p.error('Requires Windows and CubeSmoke')
    if a.mode == 'smoke' and (not re.fullmatch('[0-9a-f]{32}', a.device) or not a.bundle or (not a.bundle.is_file())):
        p.error('Smoke requires device UUID and bundle')
    if a.mode == 'describe' and (a.device != 'auto' or a.bundle or a.validation != 'required'):
        p.error('Description is device-free')
    out.mkdir(parents=True, exist_ok=False)
    summary = {'schema_version': 1, 'checkpoint': '06c', 'status': 'FAIL', 'qualification': 'NOT_RUN',
               'started_utc': datetime.now(timezone.utc).isoformat()}
    started = time.monotonic()
    code = 1
    try:
        inputs = source_inputs()
        digest = foundation.source_digest(inputs)
        lock = sha256(ROOT / 'dependencies.lock.json')
        bundle_hash = sha256(a.bundle) if a.bundle else ''
        summary.update(input_sha256=inputs, source_sha256=digest, dependency_lock_sha256=lock,
                       executable_sha256=sha256(exe), bundle_sha256=bundle_hash,
                       git_revision=command_output(['git', 'rev-parse', 'HEAD']), device=a.device, validation=a.validation)
        tool_names = ['cube_smoke.py', 'offscreen_smoke.py', 'graphics_device_smoke.py', 'graphics_probe.py',
                      'process_runner.py', 'windows_unelevated.py', 'shader_pipeline.py']
        summary['tool_inputs'] = {n: sha256(ROOT / 'tools' / n) for n in tool_names}
        validation = None
        if a.mode == 'smoke' and a.validation == 'required':
            if not a.validation_manifest:
                raise Unavailable('Required validation needs locked receipt')
            validation = validation_tools(a.validation_manifest.resolve(), lock)
            summary['validation_tools'] = validation
            summary['process_security'] = check_layer_process()
        settings = out / 'vk_layer_settings.txt'
        settings.write_text('# Required core/synchronization and NVRHI validation.\n', encoding='utf-8')
        implicit = out / 'implicit-layers-empty'
        implicit.mkdir()
        env, summary['layer_environment'] = layer_environment(validation, settings, implicit)
        (out / 'environment.json').write_text(
            json.dumps(collect(a.build_dir.resolve() if a.build_dir else None), indent=2) + '\n', encoding='utf-8')
        command = [str(exe), '--' + a.mode]
        if a.mode == 'smoke':
            command += ['--device', a.device, '--bundle', str(a.bundle.resolve()),
                        '--output', str(out / 'readbacks'), '--validation', a.validation]
        summary['command'] = command
        result = run_process(command, cwd=exe.parent, timeout=60, env=env)
        (out / 'stdout.jsonl').write_text(result.stdout, encoding='utf-8')
        (out / 'stderr.log').write_text(result.stderr, encoding='utf-8')
        rows, errors, comparisons = inspect_result(result, a.mode, a.device, a.validation, lock,
                                                   digest, bundle_hash, out / 'readbacks')
        if (source_inputs() != inputs or sha256(exe) != summary['executable_sha256']
                or (a.bundle and sha256(a.bundle) != bundle_hash)):
            errors.append('Inputs changed during execution')
        if any(sha256(ROOT / 'tools' / n) != h for n, h in summary['tool_inputs'].items()):
            errors.append('Verification tools changed')
        if validation and validation_tools(a.validation_manifest.resolve(), lock) != validation:
            errors.append('Validation inputs changed')
        summary.update(records=rows, errors=errors, comparisons=comparisons, process_exit_code=result.returncode,
                       timed_out=result.timed_out, cleanup_errors=list(result.cleanup_errors))
        for comparison in comparisons:
            name = comparison['name']
            index = int(name[-1])
            w, h = EXTENTS[index]
            reference.write_png(out / (name + '.png'), w, h, (out / 'readbacks' / (name + '.rgba8')).read_bytes())
        code = 124 if result.timed_out else 1 if errors else result.returncode
        if code == 0:
            summary['status'] = 'PASS' if a.validation == 'required' else 'NOT_RUN'
            if a.mode == 'smoke' and a.validation == 'required':
                summary['qualification'] = 'PASS'
    except Unavailable as error:
        summary.update(status='NOT_RUN', errors=[str(error)])
        code = 3
    except (OSError, ValueError, TypeError, KeyError) as error:
        summary.setdefault('errors', []).append(str(error))
    summary.update(exit_code=code, elapsed_seconds=round(time.monotonic() - started, 3),
                   finished_utc=datetime.now(timezone.utc).isoformat())
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': summary['status'], 'qualification': summary['qualification'], 'summary': str(out / 'summary.json')}))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
