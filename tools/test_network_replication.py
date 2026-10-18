"""Bounded separate-process replication qualification using public mTLS test identities."""

from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import threading
import time

from process_runner import run_process


SCENARIOS = ('convergence', 'duplicate-state', 'missing-baseline', 'ack-rejection',
             'reconnect', 'forged-state')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve()
    fixtures = args.fixtures.resolve()
    rows, scenarios, errors = [], [], []

    def execute(label, command):
        started = time.monotonic()
        result = run_process(command, timeout=18)
        row = {'name': label, 'command': list(map(str, command)),
               'elapsed_seconds': time.monotonic() - started,
               'exit_code': result.returncode, 'timed_out': result.timed_out,
               'cleanup_errors': result.cleanup_errors,
               'stdout': result.stdout, 'stderr': result.stderr}
        try:
            lines = result.stdout.splitlines()
            row['result'] = json.loads(lines[-1]) if lines else None
        except json.JSONDecodeError:
            row['result'] = None
        row['status'] = ('PASS' if result.returncode == 0 and not result.timed_out
                         and not result.cleanup_errors and isinstance(row['result'], dict)
                         and row['result'].get('status') == 'PASS' else 'FAIL')
        return row

    def readiness(path, server):
        deadline = time.monotonic() + 4
        cadence = threading.Event()
        while time.monotonic() < deadline:
            try:
                port = json.loads(path.read_text(encoding='utf-8'))['port']
                if isinstance(port, int) and 1 <= port <= 65535:
                    return port
            except (OSError, ValueError, KeyError):
                pass
            if server.done():
                raise RuntimeError(f'Replication server exited before readiness: {server.result()}')
            # Only readiness polling uses a real wait. Protocol ACKs drive scene stages.
            cadence.wait(0.01)
        raise RuntimeError('Replication server readiness deadline expired')

    def expect(result, expected, context):
        for key, value in expected.items():
            if result.get(key) != value:
                raise RuntimeError(f'{context}: expected {key}={value!r}, received {result.get(key)!r}')

    def scenario(pool, directory, name):
        forged = name == 'forged-state'
        identities = ('a',) if forged else ('a', 'b')
        ready = directory / (name + '-ready.json')
        server = pool.submit(execute, name + '.server',
                             [str(probe), '--mode', 'server', '--fixtures', str(fixtures),
                              '--ready', str(ready), '--scenario', name])
        workers, client_rows = [], []
        try:
            port = readiness(ready, server)
            workers = [pool.submit(execute, name + '.client-' + identity,
                                   [str(probe), '--mode', 'client', '--fixtures', str(fixtures),
                                    '--port', str(port), '--scenario', name,
                                    '--identity', identity]) for identity in identities]
        finally:
            # All launched children remain owned by run_process through bounded cleanup,
            # including readiness failures. Collect every result before inspecting any.
            client_rows = [worker.result(timeout=22) for worker in workers]
            server_row = server.result(timeout=22)
            rows.extend([*client_rows, server_row])
        for row in [*client_rows, server_row]:
            if row['status'] != 'PASS':
                raise RuntimeError(f"{row['name']} failed: {row['stderr']} {row['stdout']}")
        result = server_row['result']
        replay_baseline = name in ('missing-baseline', 'reconnect')
        expected = {
            'role': 'server', 'accepted_connections': 1 if forged else 3 if name == 'reconnect' else 2,
            'distinct_credentials': 1 if forged else 2,
            'scene_stage': 0 if forged else 2,
            'initial_scene_revision': 4, 'final_scene_revision': 4 if forged else 9,
            'controls': {'forged-state': 0, 'ack-rejection': 9,
                         'missing-baseline': 7, 'reconnect': 7}.get(name, 6),
            'control_rejections': 2 if name == 'ack-rejection' else 1 if name == 'reconnect' else 0,
            'duplicate_controls': int(name == 'ack-rejection'),
            'resyncs': int(name == 'missing-baseline'), 'codec_rejections': int(forged),
            'logical_updates': 1 if forged else 7 if replay_baseline else 6,
            'sent_updates': 1 if forged else 12 if name == 'duplicate-state' else 7 if replay_baseline else 6,
            'spawns': 2 if forged else 9 if replay_baseline else 6,
            'updates': 0 if forged else 4, 'despawns': 0 if forged else 2,
            'privacy_checked': True, 'live_streams': 0, 'live_sessions': 0,
        }
        expect(result, expected, name + '.server')
        payload_bytes = result.get('payload_bytes_sent')
        if (not isinstance(payload_bytes, int) or payload_bytes <= 0
                or result.get('socket_bytes_sent', 0) <= payload_bytes
                or result.get('socket_bytes_received', 0) <= 0):
            raise RuntimeError(f'{name}: missing measured payload/TLS traffic counters')
        for identity, row in zip(identities, client_rows):
            observed = row['result']
            common = {'role': 'client', 'privacy_checked': True, 'owned_results': True}
            if forged:
                expect(observed, {**common, 'forgery_rejected': True, 'applied': 1}, row['name'])
                continue
            exceptional = identity == 'a' and replay_baseline
            expect(observed, {
                **common, 'applied': 3, 'duplicates': 3 if name == 'duplicate-state' else 0,
                'resyncs': int(identity == 'a' and name == 'missing-baseline'),
                'reconnects': int(identity == 'a' and name == 'reconnect'),
                'baselines': 2 if exceptional else 1, 'deltas': 1 if exceptional else 2,
                'invalid_controls': (2 if name == 'ack-rejection' else 1 if name == 'reconnect' else 0)
                                    if identity == 'a' else 0,
                'acknowledgements': 3, 'final_entities': 2, 'final_positions': [22, 30],
                'final_revision': 4 if identity == 'a' and name == 'missing-baseline'
                                  else 2 if identity == 'a' and name == 'reconnect' else 3,
            }, row['name'])
        if not forged and sum(row['result'].get('payload_bytes_received', 0)
                              for row in client_rows) != payload_bytes:
            raise RuntimeError(f'{name}: server publications differ from actually received payload bytes')
        scenarios.append({'name': name, 'status': 'PASS', 'client_processes': len(client_rows),
                          'connections': result['accepted_connections'],
                          'logical_updates': result['logical_updates'],
                          'sent_updates': result['sent_updates'],
                          'control_rejections': result['control_rejections'],
                          'codec_rejections': result['codec_rejections'],
                          'payload_bytes_sent': payload_bytes,
                          'socket_bytes_sent': result['socket_bytes_sent'],
                          'socket_bytes_received': result['socket_bytes_received']})

    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix='shark-replication-process-') as directory:
            with ThreadPoolExecutor(max_workers=3) as pool:
                for name in SCENARIOS:
                    scenario(pool, Path(directory), name)
        if len(scenarios) != 6 or len(rows) != 17:
            raise RuntimeError('Replication scenario/process inventory mismatch')
    except Exception as error:
        errors.append(str(error))
    fixture_hashes = {path.relative_to(fixtures).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in sorted(fixtures.rglob('*.pem'))}
    if len(fixture_hashes) != 17:
        errors.append('Replication credential fixture inventory must contain exactly 17 public PEM files')
    receipt = {
        'status': 'PASS' if not errors else 'FAIL',
        'scope': 'Separate loopback processes with two enrolled mTLS identities, active Region Scene publications, owned client caches and semantic duplicate/cache-loss/reconnect faults. No TCP byte-loss simulation, spatial interest, persistence, WAN or MMO capacity claim.',
        'redaction': 'Only explicit public fixture paths and hashes, public pose checks and outcome counters are recorded; no PEM contents, private keys, session IDs or raw payloads.',
        'elapsed_seconds': time.monotonic() - started,
        'fixture_sha256': fixture_hashes,
        'scenarios': scenarios, 'processes': rows, 'errors': errors,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': receipt['status'], 'scenarios': len(scenarios), 'processes': len(rows),
                      'output': str(args.output), 'errors': errors}, indent=2))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
