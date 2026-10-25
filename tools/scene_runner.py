"""Supervise explicit CPU scene cases or existing native GPU regression fixtures.

This test entry point never renders authored scenes or provides authoring commands.
CPU work stays in the display-free SceneRunner executable. GPU work requires a
selected Windows device and the existing validated cube fixtures. No fallback.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import sys
import uuid

from process_runner import ProcessResult, run_process

ROOT = Path(__file__).resolve().parents[1]
VERSION = 1
MAX_JSON = 1024 * 1024
MAX_ARTIFACTS = 128
MAX_ARTIFACT_BYTES = 32 * 1024 * 1024
GPU_CASES = {'gpu.offscreen-cube': 'offscreen_smoke.py', 'gpu.window-cube': 'cube_smoke.py'}
TOKEN = re.compile(r'[A-Za-z0-9._-]{1,64}\Z')


class RequestError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class CapabilityUnavailable(RequestError):
    def __init__(self, message):
        super().__init__('capability_unavailable', message)


class Cancelled(BaseException):
    pass


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise RequestError('invalid_argument', message)


def require(condition, message):
    if not condition:
        raise RequestError('execution_failed', message)


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON key: ' + key)
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError('Nonfinite JSON number: ' + value)


def finite_float(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError('Nonfinite JSON float')
    return value


def decode(text):
    require(len(text.encode('utf-8')) <= MAX_JSON, 'JSON exceeds one MiB')
    return json.loads(text, object_pairs_hook=unique, parse_constant=reject_constant, parse_float=finite_float)


def read_json(path):
    require(path.is_file() and path.stat().st_size <= MAX_JSON and not path.is_symlink(),
            'Missing, linked or oversized evidence JSON: ' + str(path))
    return decode(path.read_text(encoding='utf-8'))


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def number(value, maximum):
    return type(value) is int and 0 <= value <= maximum


def no_links(path):
    # Includes dangling symlinks and Windows junctions in existing ancestors.
    for part in (path, *path.parents):
        if part.is_symlink() or (hasattr(part, 'is_junction') and part.is_junction()):
            raise RequestError('invalid_path', 'Evidence paths must not traverse links or junctions')


def publish(path, value):
    data = (json.dumps(value, ensure_ascii=False, allow_nan=False, indent=2) + '\n').encode('utf-8')
    require(len(data) <= MAX_JSON, 'Execution result exceeds one MiB')
    stage = path.parent / ('.shark-run-stage-' + uuid.uuid4().hex)
    stage.mkdir()
    temporary = stage / 'result.json'
    try:
        with temporary.open('xb') as stream:
            stream.write(data)
        os.link(temporary, path)  # Complete, create-only publication. No overwrite fallback.
    finally:
        temporary.unlink(missing_ok=True)
        stage.rmdir()
    return data.decode('utf-8')


def artifacts(root):
    result = []
    total = 0
    for path in sorted(root.rglob('*')):
        no_links(path)
        if path.is_dir():
            continue
        if path.name == 'result.json' and path.parent == root:
            continue
        require(path.is_file(), 'Unexpected evidence file type')
        relative = path.relative_to(root).as_posix()
        size = path.stat().st_size
        total += size
        require(len(relative.encode('utf-8')) <= 4096 and len(path.relative_to(root).parts) <= 8
                and len(result) < MAX_ARTIFACTS and total <= MAX_ARTIFACT_BYTES,
                'Evidence exceeds fixed file/path/byte limits')
        result.append({'path': relative, 'bytes': size, 'sha256': sha(path)})
    return result


def parse(argv):
    parser = Parser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--mode', choices=('cpu', 'gpu'), required=True)
    parser.add_argument('--case')
    parser.add_argument('--list-cases', action='store_true')
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--output', type=Path, help='Fresh evidence directory; parent must exist')
    parser.add_argument('--run-id', default='local')
    parser.add_argument('--revision', default='unspecified')
    parser.add_argument('--timeout', type=float, default=90)
    for option in ('seed', 'ticks', 'hz'):
        parser.add_argument('--' + option, type=int)
    parser.add_argument('--input', action='append', default=[])
    parser.add_argument('--project', type=Path)
    parser.add_argument('--workspace', type=Path)
    parser.add_argument('--device')
    parser.add_argument('--shaders', type=Path)
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--validation-manifest', type=Path)
    parser.add_argument('--build-dir', type=Path)
    require(len(argv) <= 4096 * 2 + 40 and all(len(a.encode('utf-8')) <= 4096 for a in argv),
            'Arguments exceed fixed count/byte bounds')
    seen = set()
    for value in argv:
        if value.startswith('--'):
            option = value.split('=', 1)[0]
            if option != '--input' and option in seen:
                raise RequestError('invalid_argument', 'Duplicate option: ' + option)
            seen.add(option)
    args = parser.parse_args(argv)
    if not TOKEN.fullmatch(args.run_id) or not TOKEN.fullmatch(args.revision):
        raise RequestError('invalid_argument', 'Run ID/revision must be bounded ASCII tokens')
    if not math.isfinite(args.timeout) or not 1 <= args.timeout <= 180:
        raise RequestError('invalid_argument', 'Timeout must be finite and in 1..180 seconds')
    if args.list_cases:
        if seen - {'--mode', '--list-cases', '--executable'}:
            raise RequestError('invalid_argument', 'Case discovery accepts only mode and CPU executable')
        if args.mode == 'gpu' and args.executable:
            raise RequestError('invalid_argument', 'GPU discovery performs no executable/device access')
        return args
    if args.output is None:
        raise RequestError('invalid_argument', 'Execution requires --output FRESH_DIRECTORY')
    if args.mode == 'cpu':
        args.case = args.case or 'foundation'
        if any(getattr(args, key) is not None for key in ('device', 'shaders', 'bundle', 'validation_manifest', 'build_dir')):
            raise RequestError('invalid_argument', 'GPU options cannot enter CPU execution')
        if (args.seed is not None and not 0 <= args.seed <= 2**64-1
                or args.ticks is not None and not 1 <= args.ticks <= 10000
                or args.hz is not None and not 1 <= args.hz <= 1000 or len(args.input) > 4096):
            raise RequestError('invalid_argument', 'CPU seed/tick/rate/input bounds exceeded')
    else:
        if args.case not in GPU_CASES or not args.device or not re.fullmatch('[0-9a-f]{32}', args.device):
            raise RequestError('invalid_argument', 'GPU execution requires a known GPU case and exact lowercase device UUID')
        if any(getattr(args, key) is not None for key in ('seed', 'ticks', 'hz', 'project', 'workspace')) or args.input:
            raise RequestError('invalid_argument', 'Fixed GPU fixtures have no seed/tick/input/project controls')
        if args.case == 'gpu.offscreen-cube' and args.bundle or args.case == 'gpu.window-cube' and args.shaders:
            raise RequestError('invalid_argument', 'Shader generation and bundle belong to different GPU cases')
    return args


def executable(args):
    if args.executable is None or not args.executable.is_file():
        raise CapabilityUnavailable('A built native executable must be provided explicitly')
    return args.executable.resolve()


def cpu_catalog(exe):
    process = run_process([str(exe), '--list-cases'], cwd=Path.cwd(), timeout=10)
    require(process.returncode == 0 and not process.timed_out and not process.cleanup_errors
            and not process.stderr.strip(), 'CPU case discovery failed')
    catalog = decode(process.stdout)
    require(isinstance(catalog, dict) and catalog.get('format') == 'shark.scene-cases'
            and type(catalog.get('schema_version')) is int and catalog['schema_version'] == 1
            and catalog.get('mode') == 'cpu', 'Unsupported CPU case catalog')
    ids = catalog.get('cases')
    require(isinstance(ids, list) and 1 <= len(ids) <= 32 and all(isinstance(v, str) and TOKEN.fullmatch(v) for v in ids)
            and len(set(ids)) == len(ids), 'Invalid CPU case IDs')
    suites = catalog.get('suites')
    require(isinstance(suites, dict) and len(suites) <= 32 and all(TOKEN.fullmatch(k) and isinstance(v, list)
            and v and all(isinstance(case, str) and case in ids for case in v) and len(set(v)) == len(v)
            for k, v in suites.items()), 'Invalid CPU suite IDs')
    return catalog


def cpu_report(process, args, expected):
    require(not process.timed_out and not process.cleanup_errors and not process.stderr.strip(),
            'CPU worker failed cleanup, exceeded deadline or emitted stderr')
    value = decode(process.stdout)
    require(isinstance(value, dict) and value.get('format') == 'shark.scene-run'
            and type(value.get('schema_version')) is int and value['schema_version'] == 1
            and value.get('renderer_initialized') is False, 'Unsupported CPU worker result')
    if process.returncode == 2:
        require(value.get('status') == 'ERROR' and value.get('mode') == 'none' and isinstance(value.get('error'), dict),
                'CPU worker error envelope contradicts its exit')
        return value
    require(process.returncode in (0, 1) and value.get('mode') == 'cpu'
            and value.get('status') == ('PASS' if process.returncode == 0 else 'FAIL'),
            'CPU worker exit/status/mode mismatch')
    require(value.get('run_id') == args.run_id and value.get('revision') == args.revision
            and type(value.get('seed')) is int and value['seed'] == (args.seed if args.seed is not None else 1337)
            and type(value.get('ticks')) is int and value['ticks'] == (args.ticks if args.ticks is not None else 60)
            and type(value.get('tick_rate_hz')) is int and value['tick_rate_hz'] == (args.hz if args.hz is not None else 60),
            'CPU worker configuration differs from request')
    expected_inputs = []
    for event in args.input:
        parts = event.split(':')
        require(len(parts) == 3 and parts[1] == 'offset', 'CPU worker accepted an unknown input action')
        tick, amount = int(parts[0]), int(parts[2])
        require(0 <= tick < value['ticks'] and -(2**31) <= amount < 2**31,
                'CPU worker accepted an out-of-range input')
        expected_inputs.append({'tick':tick,'action':'offset','value':amount})
    inputs = value.get('inputs')
    require(isinstance(inputs, list) and all(isinstance(event, dict) and type(event.get('tick')) is int
            and type(event.get('value')) is int for event in inputs) and inputs == expected_inputs,
            'CPU worker ordered inputs differ from request')
    cases = value.get('cases')
    require(isinstance(cases, list) and [c.get('id') for c in cases if isinstance(c, dict)] == expected,
            'CPU worker omitted or substituted a case')
    failed = False
    for case in cases:
        require(case.get('status') in ('PASS', 'FAIL') and number(case.get('assertions'), 16384)
                and number(case.get('ticks_completed'), value['ticks']) and number(case.get('random_draws'), 65536),
                'Invalid native case status/counters')
        for key in ('initial_revision', 'final_revision'):
            revision = case.get(key)
            require(isinstance(revision, dict) and number(revision.get('instance'), 2**64-1)
                    and revision['instance'] > 0 and number(revision.get('value'), 2**64-1), 'Invalid local scene revision')
        if case['initial_revision']['instance'] == case['final_revision']['instance']:
            require(case['final_revision']['value'] >= case['initial_revision']['value'],
                    'CPU worker revision moved backwards within one incarnation')
        if case['status'] == 'PASS':
            require(case.get('failure') is None and case['assertions'] > 0 and case['ticks_completed'] == value['ticks'],
                    'Passing CPU case lacks completed work')
        else:
            require(isinstance(case.get('failure'), dict), 'Failed CPU case lacks context')
            failed = True
    require(failed == (process.returncode == 1), 'Aggregate CPU result contradicts its cases')
    return value


def gpu_preflight(args):
    if sys.platform != 'win32':
        raise CapabilityUnavailable('Native GPU cases require Windows; CPU execution is separate')
    exe = executable(args)
    if args.validation_manifest is None or not args.validation_manifest.is_file():
        raise CapabilityUnavailable('GPU cases require a locked validation-tools receipt')
    payload = args.shaders if args.case == 'gpu.offscreen-cube' else args.bundle
    if payload is None or not payload.exists():
        raise CapabilityUnavailable('GPU case requires its verified precompiled shader payload')
    # Imports stay on this branch. CPU discovery/execution never loads graphics helpers.
    import graphics_probe
    import offscreen_smoke
    import cube_smoke
    security = graphics_probe.check_layer_process()
    lock = sha(ROOT/'dependencies.lock.json')
    validation = graphics_probe.validation_tools(args.validation_manifest.resolve(), lock)
    if args.case == 'gpu.offscreen-cube':
        manifest, _, _ = offscreen_smoke.shader_inputs(payload.resolve())
    else:
        require(payload.is_file() and 32 <= payload.stat().st_size <= 8 * 1024 * 1024, 'Invalid shader bundle size')
        manifest = {'bundle_sha256': sha(payload)}
    return exe, {'dependency_lock_sha256': lock, 'validation_receipt_sha256': sha(args.validation_manifest),
                 'validation': validation, 'process_security': security, 'shader_payload': manifest}


def gpu_report(summary, process, args, directory, preflight):
    import offscreen_smoke
    import cube_smoke
    module = offscreen_smoke if args.case == 'gpu.offscreen-cube' else cube_smoke
    require(isinstance(summary, dict) and type(summary.get('schema_version')) is int and summary['schema_version'] == 1,
            'Unsupported GPU fixture receipt')
    if process.returncode == 3 and summary.get('status') == 'NOT_RUN':
        raise CapabilityUnavailable('Native GPU fixture reported unavailable required capabilities')
    if process.returncode != 0:
        # Existing fixture wrappers retain native capability errors even when
        # their strict complete-workload parser rejects the incomplete stream.
        # Classify that honest unavailable outcome without selecting any other
        # device, mode or validation setting.
        path = directory/'stdout.jsonl'
        if path.is_file() and path.stat().st_size <= MAX_JSON:
            rows = [decode(line) for line in path.read_text(encoding='utf-8').splitlines()]
            if len(rows) >= 2 and all(isinstance(row, dict) for row in rows):
                identity, failure = rows[0], rows[-1]
                error = failure.get('error')
                if (identity.get('kind') == 'identity' and identity.get('selection') == args.device
                        and identity.get('validation') == 'required'
                        and identity.get('dependency_lock_sha256') == preflight['dependency_lock_sha256']
                        and identity.get('source_sha256') == module.foundation.source_digest(module.source_inputs())
                        and failure.get('kind') == 'failure' and isinstance(error, dict)
                        and error.get('code') == 'capability_unavailable'):
                    raise CapabilityUnavailable(str(error.get('message', 'Native capability unavailable')))
    require(process.returncode == 0 and summary.get('exit_code') == 0 and summary.get('status') == 'PASS'
            and summary.get('qualification') == 'PASS' and summary.get('validation') == 'required',
            'GPU fixture did not complete required validated work')
    require(summary.get('dependency_lock_sha256') == preflight['dependency_lock_sha256']
            and summary.get('executable_sha256') == sha(args.executable.resolve()), 'GPU fixture input identity mismatch')
    inputs = module.source_inputs()
    digest = module.foundation.source_digest(inputs)
    require(summary.get('input_sha256') == inputs and summary.get('source_sha256') == digest,
            'GPU fixture source identity is stale')
    raw = directory/'stdout.jsonl'
    errors = directory/'stderr.log'
    require(raw.is_file() and errors.is_file() and raw.stat().st_size <= MAX_JSON and errors.stat().st_size <= MAX_JSON,
            'Missing or oversized native graphics logs')
    native = ProcessResult(summary.get('command', []), summary.get('process_exit_code'),
                           raw.read_text(encoding='utf-8'), errors.read_text(encoding='utf-8'),
                           summary.get('timed_out'), tuple(summary.get('cleanup_errors', [])))
    require(type(native.returncode) is int and type(native.timed_out) is bool
            and not native.timed_out and not native.cleanup_errors, 'Invalid native completion metadata')
    if args.case == 'gpu.offscreen-cube':
        rows, problems, comparisons = offscreen_smoke.inspect_result(native, 'render', args.device, 'required',
            preflight['dependency_lock_sha256'], digest, directory/'readbacks')
        expected_count = 2
    else:
        rows, problems, comparisons = cube_smoke.inspect_result(native, 'smoke', args.device, 'required',
            preflight['dependency_lock_sha256'], digest, sha(args.bundle.resolve()), directory/'readbacks')
        expected_count = 6
    require(not problems and len(comparisons) == expected_count and all(c.get('status') == 'PASS' for c in comparisons),
            'Independent GPU evidence validation failed: ' + '; '.join(problems))
    require(summary.get('records') == rows and summary.get('comparisons') == comparisons,
            'GPU fixture receipt disagrees with raw native evidence')
    # inspect_result also checks explicit UUID/capabilities, fixed color/depth
    # tolerances, new submissions, clean validation and completed retirement.
    return {'adapter': rows[1]['adapter'], 'validation': 'Vulkan core/synchronization and NVRHI required',
            'comparisons': comparisons, 'native_complete': rows[-1], 'source_sha256': digest}


def execute(args, report, output):
    tool_names = ['scene_runner.py', 'process_runner.py']
    if args.mode == 'gpu':
        tool_names += ['offscreen_smoke.py', 'cube_smoke.py', 'graphics_device_smoke.py',
                       'graphics_probe.py', 'shader_pipeline.py', 'toolchain_report.py', 'windows_unelevated.py']
    report['tool_inputs'] = {name:sha(ROOT/'tools'/name) for name in tool_names}
    exe = executable(args) if args.mode == 'cpu' else None
    preflight = None
    if args.mode == 'gpu':
        try:
            exe, preflight = gpu_preflight(args)
        except Exception as error:
            # Preserve the existing capability classification without importing
            # graphics modules on CPU or non-Windows paths.
            if type(error).__name__ == 'Unavailable':
                raise CapabilityUnavailable(str(error)) from error
            raise
        report['gpu_prerequisites'] = preflight
    before = sha(exe)
    if args.mode == 'cpu':
        catalog = cpu_catalog(exe)
        if args.case in catalog['cases']:
            expected = [args.case]
        elif args.case in catalog['suites']:
            expected = catalog['suites'][args.case]
        else:
            raise RequestError('invalid_argument', 'Unknown CPU case/suite')
        command = [str(exe), '--mode', 'cpu', '--case', args.case, '--run-id', args.run_id,
                   '--revision', args.revision, '--output', str(output/'native-result.json')]
        for key in ('seed', 'ticks', 'hz', 'project', 'workspace'):
            value = getattr(args, key)
            if value is not None:
                command += ['--'+key, str(value.resolve() if isinstance(value, Path) else value)]
        for event in args.input:
            command += ['--input', event]
        directory = None
    else:
        directory = output/'native'
        command = [sys.executable, str(ROOT/'tools'/GPU_CASES[args.case]), '--executable', str(exe),
                   '--mode', 'render' if args.case == 'gpu.offscreen-cube' else 'smoke', '--device', args.device,
                   '--validation', 'required', '--validation-manifest', str(args.validation_manifest.resolve()),
                   '--output', str(directory)]
        key = 'shaders' if args.case == 'gpu.offscreen-cube' else 'bundle'
        command += ['--'+key, str(getattr(args, key).resolve())]
        if args.build_dir:
            command += ['--build-dir', str(args.build_dir.resolve())]
    report['command'] = command
    report['executable_sha256'] = before
    if args.mode == 'gpu':
        # A failed/interrupted child may already have initialized its device.
        # Only verified native completion can affirm initialization; never turn
        # missing/partial evidence into a false device-free claim.
        report['renderer_initialized'] = None
    process = run_process(command, cwd=Path.cwd(), timeout=args.timeout)
    require(len(process.stdout.encode('utf-8')) <= MAX_JSON and len(process.stderr.encode('utf-8')) <= MAX_JSON,
            'Worker output exceeds fixed limits')
    (output/'worker-stdout.log').write_text(process.stdout, encoding='utf-8')
    (output/'worker-stderr.log').write_text(process.stderr, encoding='utf-8')
    report['process'] = {'exit_code': process.returncode, 'timed_out': process.timed_out,
                         'cleanup_errors': list(process.cleanup_errors)}
    if process.timed_out:
        report['status'] = 'FAIL'
        report['error'] = {'code':'execution_failed', 'message':'Worker deadline expired; work may have executed',
                           'operation':'scene.execution.worker'}
        return 124
    require(not process.cleanup_errors and not process.stderr.strip(), 'Worker cleanup or stderr failure')
    require(before == sha(exe), 'Executable changed during execution')
    require(all(sha(ROOT/'tools'/name) == digest for name,digest in report['tool_inputs'].items()),
            'Supervisor/evidence tools changed during execution')
    if args.mode == 'cpu':
        native = cpu_report(process, args, expected)
        if process.returncode in (0, 1):
            require(read_json(output/'native-result.json') == native, 'Published native result differs from stdout')
        report['cpu'] = native
        report['status'] = native['status']
        return process.returncode
    # Check sizes/links before reading and rasterizing native data.
    artifacts(output)
    summary = read_json(directory/'summary.json')
    report['gpu'] = gpu_report(summary, process, args, directory, preflight)
    require(gpu_preflight(args)[1] == preflight, 'GPU prerequisites changed during execution')
    report['renderer_initialized'] = True
    report['status'] = 'PASS'
    return 0


def main(argv=None):
    report = {'format':'shark.scene-execution', 'schema_version':VERSION, 'status':'ERROR', 'mode':'none',
              'renderer_initialized':False, 'artifacts':[], 'started_utc':datetime.now(timezone.utc).isoformat()}
    output = None
    code = 2
    try:
        args = parse(sys.argv[1:] if argv is None else argv)
        report.update(mode=args.mode, case=args.case, run_id=args.run_id, revision=args.revision)
        report['scope'] = ('Native CPU authored-scene cases' if args.mode == 'cpu' else
                           'Fixed native graphics fixtures; authored scene rendering is not exercised')
        if args.mode == 'gpu' and not args.list_cases:
            report['requested_device'] = args.device
        if args.list_cases:
            catalog = cpu_catalog(executable(args)) if args.mode == 'cpu' else {
                'cases':list(GPU_CASES), 'suites':{}, 'requires':['Windows native graphics fixture',
                    'explicit device UUID', 'verified precompiled shaders', 'required Vulkan/NVRHI validation']}
            print(json.dumps({'format':'shark.scene-execution-cases','schema_version':VERSION,
                              'mode':args.mode,'renderer_initialized':False,'catalog':catalog}))
            return 0
        output = args.output.absolute()
        no_links(output)
        if output.exists():
            raise RequestError('conflict', 'Evidence directory already exists; preserved')
        require(output.parent.is_dir(), 'Evidence parent directory must exist')
        output.mkdir()
        # Once owned, expected failures leave a result and any partial evidence.
        report['artifact_root'] = str(output)
        code = execute(args, report, output)
    except CapabilityUnavailable as error:
        report.update(status='NOT_RUN', error={'code':error.code,'message':str(error)[:1024],
                                               'operation':'scene.execution.capability'})
        code = 3
    except (KeyboardInterrupt, Cancelled):
        report.update(status='CANCELLED', error={'code':'execution_failed','message':'Execution cancelled; work may have executed',
                                                'operation':'scene.execution.cancel'})
        code = 130
    except (OSError, ValueError, TypeError, KeyError, RecursionError, RequestError) as error:
        report.update(status='FAIL' if 'command' in report else 'ERROR',
                      error={'code':getattr(error, 'code', 'io_failure' if isinstance(error, OSError) else 'execution_failed'),
                             'message':str(error)[:1024], 'operation':'scene.execution'})
        code = 1 if 'command' in report else 2
    report.update(exit_code=code, finished_utc=datetime.now(timezone.utc).isoformat())
    try:
        if output is not None and 'artifact_root' in report:
            report['artifacts'] = artifacts(output)
            text = publish(output/'result.json', report)
        else:
            text = json.dumps(report, ensure_ascii=False, allow_nan=False) + '\n'
        sys.stdout.write(text)
        sys.stdout.flush()
    except (OSError, ValueError, RequestError) as error:
        fallback = {'format':'shark.scene-execution','schema_version':VERSION,'status':'ERROR','mode':report['mode'],
                    'renderer_initialized':report['renderer_initialized'],'exit_code':2,
                    'error':{'code':'io_failure','message':str(error)[:1024],'operation':'scene.execution.publish'}}
        try:
            sys.stdout.write(json.dumps(fallback)+'\n'); sys.stdout.flush()
        except (OSError, ValueError):
            pass
        return 2
    return code


if __name__ == '__main__':
    # Windows pipes otherwise inherit the legacy console code page. The wire
    # protocol and published results are UTF-8 even for Unicode project paths.
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    def cancel(_signum, _frame):
        raise Cancelled()
    signal.signal(signal.SIGTERM, cancel)
    if hasattr(signal, 'SIGBREAK'):
        signal.signal(signal.SIGBREAK, cancel)
    raise SystemExit(main())
