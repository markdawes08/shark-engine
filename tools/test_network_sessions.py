"""Bounded separate-process authentication/session qualification using public test keys."""

from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import threading
import time

from process_runner import run_process


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
        result = run_process(command, timeout=12)
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
                raise RuntimeError(f'Session server exited before readiness: {server.result()}')
            # Bounded process readiness polling, independent of deterministic unit clocks.
            cadence.wait(0.01)
        raise RuntimeError('Session server readiness deadline expired')

    def scenario(pool, directory, name, identities=('a',), *, connections=1,
                 tls_rejections=0, codec_rejections=0, requests=None):
        ready = directory / (name + '-ready.json')
        server_command = [str(probe), '--mode', 'server', '--fixtures', str(fixtures),
                          '--ready', str(ready), '--scenario', name,
                          '--connections', str(connections)]
        server = pool.submit(execute, name + '.server', server_command)
        client_rows = []
        try:
            port = readiness(ready, server)
            workers = [pool.submit(execute, name + '.client-' + identity,
                                   [str(probe), '--mode', 'client', '--fixtures', str(fixtures),
                                    '--port', str(port), '--scenario', name,
                                    '--identity', identity]) for identity in identities]
            client_rows = [worker.result(timeout=14) for worker in workers]
        finally:
            # Every child is owned by process_runner and reaches its watchdog/cleanup
            # even if readiness or a client result is invalid.
            server_row = server.result(timeout=14)
            rows.extend([*client_rows, server_row])
        for row in [*client_rows, server_row]:
            if row['status'] != 'PASS':
                raise RuntimeError(f"{row['name']} failed: {row['stderr']} {row['stdout']}")
        result = server_row['result']
        if (result.get('accepted_connections') != connections
                or result.get('tls_rejections') != tls_rejections
                or result.get('codec_rejections') != codec_rejections
                or result.get('authenticated_connections') != connections - tls_rejections):
            raise RuntimeError(f'{name}: unexpected transport/codec boundary counts: {result}')
        rejection_operations = result.get('tls_rejection_operations')
        if (not isinstance(rejection_operations, list) or len(rejection_operations) != tls_rejections
                or any(operation not in ('net.tls.handshake', 'net.tls.client-profile')
                       for operation in rejection_operations)):
            raise RuntimeError(f'{name}: credential rejection did not occur at server TLS validation: {result}')
        if requests is not None and result.get('requests') != requests:
            raise RuntimeError(f'{name}: unexpected typed request count: {result}')
        admissions = {
            'two-identities': (8, 0), 'unregistered': (0, 1), 'revoked': (0, 1),
            'wrong-owner': (0, 2), 'replay': (3, 1), 'malformed': (0, 0),
            'version': (0, 0), 'expiry': (1, 1), 'live-revocation': (1, 1),
            'rate-limit': (3, 1), 'reconnect': (4, 2), 'cross-connection': (6, 1),
        }
        successful, rejected = admissions.get(name, (0, 0))
        if (result.get('successful_requests') != successful
                or result.get('rejected_requests') != rejected
                or result.get('live_sessions') != 0):
            raise RuntimeError(f'{name}: unexpected session admission/teardown counts: {result}')
        if name == 'two-identities' and result.get('distinct_credentials') != 2:
            raise RuntimeError('Two real client processes did not authenticate distinct credentials')
        for row in client_rows:
            if tls_rejections:
                if (not row['result'].get('expected_tls_rejection')
                        or row['result'].get('ready')
                        or row['result'].get('error_operation') not in
                        ('net.tls.handshake', 'net.tls.read', 'net.tls.write', 'net.tls.verify')):
                    raise RuntimeError(f'{name}: invalid credential reached readiness or failed outside TLS')
            elif codec_rejections:
                if not row['result'].get('ready') or not row['result'].get('codec_rejected'):
                    raise RuntimeError(f'{name}: malformed input did not fail at the session codec boundary')
            elif not row['result'].get('ready') or not row['result'].get('owned_results'):
                raise RuntimeError(f'{name}: missing ready/owned-result evidence')
            if name in ('reconnect', 'cross-connection') and not row['result'].get('fenced'):
                raise RuntimeError(f'{name}: missing replaced/foreign authority rejection evidence')
        scenarios.append({'name': name, 'status': 'PASS', 'client_processes': len(client_rows),
                          'connections': connections, 'tls_rejections': tls_rejections,
                          'tls_rejection_operations': rejection_operations,
                          'codec_rejections': codec_rejections, 'requests': result.get('requests')})

    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix='shark-session-process-') as directory:
            with ThreadPoolExecutor(max_workers=3) as pool:
                work = Path(directory)
                scenario(pool, work, 'two-identities', ('a', 'b'), connections=2, requests=8)
                for identity in ('absent', 'wrong-ca', 'expired', 'wrong-eku', 'no-eku',
                                 'wrong-ku', 'ca-client'):
                    scenario(pool, work, 'credential-' + identity, (identity,), tls_rejections=1,
                             requests=0)
                for name in ('unregistered', 'revoked'):
                    scenario(pool, work, name, requests=1)
                scenario(pool, work, 'wrong-owner', requests=2)
                scenario(pool, work, 'replay', requests=4)
                scenario(pool, work, 'malformed', codec_rejections=1, requests=0)
                scenario(pool, work, 'version', codec_rejections=1, requests=0)
                scenario(pool, work, 'expiry', requests=2)
                scenario(pool, work, 'live-revocation', requests=2)
                scenario(pool, work, 'rate-limit', requests=4)
                scenario(pool, work, 'reconnect', connections=2, requests=6)
                scenario(pool, work, 'cross-connection', connections=2, requests=7)
    except Exception as error:
        errors.append(str(error))
    receipt = {
        'status': 'PASS' if not errors else 'FAIL',
        'scope': 'Separate local processes, verified client certificates, enrolled player identity and owned session controls. No persistence, replication, administrative authority, WAN or MMO capacity claim.',
        'redaction': 'Only explicit public fixture paths and hashes plus outcome counters are recorded; no PEM contents, private keys, session IDs or raw request/reply payloads.',
        'elapsed_seconds': time.monotonic() - started,
        'fixture_sha256': {path.relative_to(fixtures).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in sorted(fixtures.rglob('*.pem'))},
        'scenarios': scenarios,
        'processes': rows,
        'errors': errors,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': receipt['status'], 'scenarios': len(scenarios), 'processes': len(rows),
                      'output': str(args.output), 'errors': errors}, indent=2))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
