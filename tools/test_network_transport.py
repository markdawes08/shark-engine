"""Exercise real loopback TLS server/client processes using the public transport API."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import socket
import ssl
import struct
import tempfile
import threading
import time

from process_runner import run_process


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve()
    fixtures = args.fixtures.resolve()
    rows = []
    errors = []

    def execute(label, command):
        result = run_process(command, timeout=10)
        row = {'name': label, 'command': list(map(str, command)),
               'exit_code': result.returncode, 'timed_out': result.timed_out,
               'cleanup_errors': result.cleanup_errors,
               'stdout': result.stdout, 'stderr': result.stderr}
        lines = result.stdout.splitlines()
        try:
            row['result'] = json.loads(lines[-1]) if lines else None
        except json.JSONDecodeError:
            row['result'] = None
        row['status'] = ('PASS' if result.returncode == 0 and not result.timed_out
                         and not result.cleanup_errors and row['result']
                         and row['result'].get('status') == 'PASS' else 'FAIL')
        return row

    def client_command(port, *, ca='ca.pem', name='shark.test', tag=1, failure=False,
                       handshake_ms=2000, graceful=False):
        return [str(probe), '--mode', 'client', '--port', str(port), '--ca', str(fixtures / ca),
                '--name', name, '--tag', str(tag), '--expect-failure', str(int(failure)),
                '--handshake-ms', str(handshake_ms), '--expect-graceful', str(int(graceful))]

    def interoperability(pool, label, *, failure_operation=None):
        # Independent standard-library TLS peer and explicit network-order codec.
        # It is an interoperability peer in the coordinator, not an engine server.
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.set_ecdh_curve('prime256v1')
        context.load_cert_chain(str(fixtures / 'server.pem'), str(fixtures / 'server-key.pem'))

        def header(kind, sequence, size, version=1):
            return struct.pack('!4sHBBQI', b'SHRK', version, kind, 0, sequence, size)

        def exact(peer, count):
            result = bytearray()
            while len(result) < count:
                part = peer.recv(count - len(result))
                if not part:
                    raise RuntimeError('Engine peer closed before the expected wire input')
                result.extend(part)
            return bytes(result)

        def frame(peer, expected_kind, expected_sequence):
            magic, version, kind, flags, sequence, size = struct.unpack('!4sHBBQI', exact(peer, 20))
            if ((magic, version, kind, flags, sequence) !=
                    (b'SHRK', 1, expected_kind, 0, expected_sequence) or size > 4096):
                raise RuntimeError('Engine peer emitted an invalid negotiated wire frame')
            return exact(peer, size)

        def serve(listener):
            try:
                raw, _ = listener.accept()
                raw.settimeout(4)
                with context.wrap_socket(raw, server_side=True) as peer:
                    version = peer.version()
                    cipher = peer.cipher()
                    if version != 'TLSv1.3':
                        raise RuntimeError('Interoperability peer did not negotiate TLS 1.3')
                    if frame(peer, 1, 1):
                        raise RuntimeError('Hello payload must be empty')
                    if label == 'wire-version':
                        peer.sendall(header(2, 1, 0, version=2))
                    else:
                        peer.sendall(header(2, 1, 0))
                        payload = frame(peer, 3, 2)
                        if label == 'wire-replay':
                            peer.sendall(header(4, 1, len(payload)) + payload)
                        elif label == 'wire-kind':
                            peer.sendall(header(255, 2, len(payload)) + payload)
                        elif label == 'wire-oversize':
                            peer.sendall(header(4, 2, 4097))
                        elif label == 'wire-partial-eof':
                            peer.sendall(header(4, 2, len(payload)) + payload[:3])
                        else:
                            peer.sendall(header(4, 2, len(payload)) + payload)
                    shutdown = 'clean'
                    try:
                        plain = peer.unwrap()  # Sends a real TLS close_notify before waiting.
                        plain.close()
                    except (ssl.SSLError, ConnectionResetError) as error:
                        # Engine close is a documented bounded local abort, so it
                        # need not return close_notify to this independent peer.
                        shutdown = type(error).__name__
                return {'name': label + '.python-tls-peer', 'status': 'PASS',
                        'tls_version': version, 'cipher': cipher, 'shutdown_result': shutdown}
            except Exception as error:
                return {'name': label + '.python-tls-peer', 'status': 'FAIL', 'error': str(error)}

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(5)
            server = pool.submit(serve, listener)
            row = execute(label + '.client', client_command(
                listener.getsockname()[1], failure=bool(failure_operation),
                graceful=not failure_operation))
            peer_row = server.result(timeout=7)
            rows.extend([row, peer_row])
        if row['status'] != 'PASS' or peer_row['status'] != 'PASS':
            raise RuntimeError(f'TLS interoperability case failed: {row} {peer_row}')
        if failure_operation:
            if row['result'].get('error_operation') != failure_operation:
                raise RuntimeError(f'Malformed wire data failed at the wrong boundary: {row}')
        elif not row['result'].get('graceful_close') or row['result'].get('echoes') != 1:
            raise RuntimeError(f'Clean TLS EOF lost the complete queued reply: {row}')

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
                raise RuntimeError(f'Server exited before readiness: {server.result()}')
            # Bounded readiness polling for actual processes, not simulated time.
            cadence.wait(0.01)
        raise RuntimeError('Server readiness deadline expired')

    def scenario(pool, work, label, *, clients=1, certificate='server.pem', ca='ca.pem',
                 name='shark.test', behavior='echo', failure=False, operation=None):
        ready = work / (label + '-ready.json')
        expected_errors = 1 if failure and behavior == 'echo' else 0
        command = [str(probe), '--mode', 'server', '--cert', str(fixtures / certificate),
                   '--key', str(fixtures / 'server-key.pem'), '--ready', str(ready),
                   '--clients', str(clients), '--expected-errors', str(expected_errors),
                   '--behavior', behavior]
        server = pool.submit(execute, label + '.server', command)
        port = readiness(ready, server)
        workers = [pool.submit(execute, label + f'.client-{index + 1}', client_command(
            port, ca=ca, name=name, tag=index + 1, failure=failure,
            handshake_ms=200 if behavior == 'stall' else 2000)) for index in range(clients)]
        client_rows = [worker.result(timeout=12) for worker in workers]
        server_row = server.result(timeout=12)
        rows.extend([*client_rows, server_row])
        for row in client_rows:
            if row['status'] != 'PASS':
                raise RuntimeError(f"{row['name']} failed: {row['stderr']} {row['stdout']}")
            result = row['result']
            if failure:
                if not result.get('expected_failure') or result.get('ready'):
                    raise RuntimeError(f"{row['name']} did not reject before readiness")
                if operation and not result.get('error_operation', '').startswith(operation):
                    raise RuntimeError(f"{row['name']} failed at the wrong boundary: {result}")
            elif (result.get('echoes') != 1 or not result.get('owned_payload')
                  or result.get('frames_sent') != 2 or result.get('frames_received') != 2):
                raise RuntimeError(f"{row['name']} is missing wire/lifetime evidence")
        row = server_row
        if row['status'] != 'PASS':
            raise RuntimeError(f"{row['name']} failed: {row['stderr']} {row['stdout']}")
        if row['result'].get('accepted') != clients:
            raise RuntimeError(f"{row['name']} accepted an unexpected process count")

    try:
        with tempfile.TemporaryDirectory(prefix='shark-network-process-') as directory:
            with ThreadPoolExecutor(max_workers=3) as pool:
                work = Path(directory)
                scenario(pool, work, 'two-clients', clients=2)
                scenario(pool, work, 'wrong-ca', ca='wrong-ca.pem', failure=True, operation='net.tls.')
                scenario(pool, work, 'wrong-name', name='other.test', failure=True, operation='net.tls.')
                scenario(pool, work, 'expired', certificate='expired-server.pem', failure=True,
                         operation='net.tls.')
                scenario(pool, work, 'peer-eof', behavior='close', failure=True)
                scenario(pool, work, 'handshake-timeout', behavior='stall', failure=True,
                         operation='net.transport.timeout')
                # A reserved non-listening socket prevents a port-reuse race.
                # Native Windows reports WSAECONNREFUSED after approximately two
                # seconds here. Allow the OS response, and retain a legitimate
                # bounded transport deadline if the platform does not report it.
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserved:
                    reserved.bind(('127.0.0.1', 0))
                    row = execute('refused.client', client_command(
                        reserved.getsockname()[1], failure=True, handshake_ms=5000))
                    rows.append(row)
                    if (row['status'] != 'PASS' or row['result'].get('ready')
                            or not row['result'].get('expected_failure')
                            or row['result'].get('error_operation') not in
                            ('net.transport.connect', 'net.transport.timeout')):
                        raise RuntimeError(f'Refused connection did not produce a bounded failure: {row}')
                interoperability(pool, 'clean-eof')
                interoperability(pool, 'wire-partial-eof', failure_operation='net.wire.finish')
                interoperability(pool, 'wire-version', failure_operation='net.wire.decode')
                interoperability(pool, 'wire-replay', failure_operation='net.sequence')
                interoperability(pool, 'wire-kind', failure_operation='net.wire.decode')
                interoperability(pool, 'wire-oversize', failure_operation='net.wire.decode')
    except Exception as error:
        errors.append(str(error))
    receipt = {
        'status': 'PASS' if not errors else 'FAIL',
        'scope': 'Real separate loopback processes; TLS 1.3 server authentication and diagnostic wire negotiation/echo. No player authentication, replication, WAN or MMO capacity claim.',
        'fault_scope': 'Real stream stall/EOF/refusal only. Seeded message/datagram faults are covered separately by core_net_fault_tests.cpp.',
        'fixture_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in sorted(fixtures.glob('*.pem'))},
        'processes': [row for row in rows if 'command' in row],
        'interoperability_peers': [row for row in rows if 'command' not in row],
        'errors': errors,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': receipt['status'], 'processes': len(receipt['processes']),
                      'interoperability_peers': len(receipt['interoperability_peers']),
                      'output': str(args.output), 'errors': errors}, indent=2))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
