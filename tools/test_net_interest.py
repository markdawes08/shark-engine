"""Measure bounded per-recipient interest traffic in actual local authenticated processes."""

from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import math
from pathlib import Path
import select
import socket
import sys
import tempfile
import threading
import time

from process_runner import run_process


SCENARIOS = ('crowd-churn-budget', 'policy-revocation', 'head-of-line')


def proxy_worker(args):
    """A bounded transparent TCP relay. Endpoints retain end-to-end mutual TLS."""
    started = time.monotonic()
    deadline = started + 40
    cap, rate = 16384, 8192
    buffered = [bytearray(), bytearray()]
    received, forwarded = [0, 0], [0, 0]
    eof, half_closed = [False, False], [False, False]
    peak = 0
    armed = False
    hold_start = None
    hold_end = None
    first_shaped_write = None
    last_shaped_write = None
    shaped_bytes = 0
    tokens = 0.0
    token_time = started
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(5)
        args.ready.write_text(json.dumps({'port': listener.getsockname()[1]}) + '\n', encoding='utf-8')
        client, _ = listener.accept()
        with client, socket.create_connection(('127.0.0.1', args.upstream_port), timeout=5) as upstream:
            # Direction0 client->server, direction1 server->client.
            sources, destinations = (client, upstream), (upstream, client)
            for connection in sources:
                connection.setblocking(False)
            while time.monotonic() < deadline:
                now = time.monotonic()
                if not armed and (args.control / 'arm').exists() and not buffered[1]:
                    armed = True
                    (args.control / 'armed').write_text('ready\n', encoding='utf-8')
                if hold_end is not None and now >= hold_end:
                    tokens = min(4096.0, tokens + (now - max(token_time, hold_end)) * rate)
                    token_time = now
                reading = [sources[n] for n in range(2) if not eof[n] and len(buffered[n]) < cap]
                writing = [destinations[n] for n in range(2) if buffered[n]
                           and (n == 0 or not armed or (hold_end is not None and now >= hold_end and tokens >= 1))]
                readable, writable, _ = select.select(reading, writing, [], .005)
                for n in range(2):
                    if sources[n] in readable:
                        try:
                            data = sources[n].recv(min(4096, cap - len(buffered[n])))
                        except BlockingIOError:
                            continue
                        except (ConnectionResetError, ConnectionAbortedError):
                            data = b''
                        if not data:
                            eof[n] = True
                        else:
                            buffered[n].extend(data)
                            received[n] += len(data)
                            peak = max(peak, len(buffered[n]))
                            if n == 1 and armed and hold_start is None:
                                hold_start = time.monotonic()
                                hold_end = hold_start + .1
                                token_time = hold_end
                    if destinations[n] in writable:
                        allowance = len(buffered[n])
                        if n == 1 and armed:
                            allowance = min(allowance, int(tokens))
                        if allowance:
                            try:
                                sent = destinations[n].send(buffered[n][:allowance])
                            except BlockingIOError:
                                sent = 0
                            if sent:
                                del buffered[n][:sent]
                                forwarded[n] += sent
                                if n == 1 and armed:
                                    tokens -= sent
                                    shaped_bytes += sent
                                    last_shaped_write = time.monotonic()
                                    if first_shaped_write is None:
                                        first_shaped_write = last_shaped_write
                    if eof[n] and not buffered[n] and not half_closed[n]:
                        try:
                            destinations[n].shutdown(socket.SHUT_WR)
                        except OSError:
                            pass
                        half_closed[n] = True
                if all(eof) and not any(buffered):
                    break
            else:
                raise RuntimeError('Transparent relay watchdog expired')
    if (not armed or hold_start is None or first_shaped_write is None or last_shaped_write is None
            or shaped_bytes < 3454 or peak > cap or received != forwarded):
        raise RuntimeError('Transparent relay did not exercise the bounded byte-preserving schedule')
    hold_ms = (first_shaped_write - hold_start) * 1000
    if hold_ms < 100 or hold_ms > 1000:
        raise RuntimeError('Transparent relay hold duration outside declared bounds')
    shaped_elapsed_ms = (last_shaped_write - hold_start) * 1000
    if shaped_bytes > rate * max(0, shaped_elapsed_ms - 100) / 1000 + 8:
        raise RuntimeError('Measured relay forwarding exceeded its fixed byte rate')
    print(json.dumps({'status': 'PASS', 'role': 'proxy', 'received_bytes': received,
                      'forwarded_bytes': forwarded, 'peak_buffer_bytes': peak,
                      'hold_ms': hold_ms, 'shaped_bytes': shaped_bytes,
                      'shaped_elapsed_ms': shaped_elapsed_ms,
                      'rate_bytes_per_second': rate, 'buffer_cap_bytes': cap,
                      'end_to_end_tls': True, 'byte_corruption': False}))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--proxy-worker', action='store_true')
    parser.add_argument('--upstream-port', type=int)
    parser.add_argument('--ready', type=Path)
    parser.add_argument('--control', type=Path)
    args = parser.parse_args()
    if args.proxy_worker:
        try:
            return proxy_worker(args)
        except Exception as error:
            print(str(error), file=sys.stderr)
            print(json.dumps({'status': 'FAIL', 'role': 'proxy', 'error': 'interest-relay'}))
            return 1
    if args.probe is None or args.fixtures is None or args.output is None:
        parser.error('--probe, --fixtures and --output are required')
    probe, fixtures = args.probe.resolve(), args.fixtures.resolve()
    rows, scenarios, errors = [], [], []

    def execute(name, command):
        begun = time.monotonic()
        result = run_process(command, timeout=45)
        row = {'name': name, 'command': list(map(str, command)),
               'elapsed_seconds': time.monotonic() - begun, 'exit_code': result.returncode,
               'timed_out': result.timed_out, 'cleanup_errors': result.cleanup_errors,
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

    def readiness(path, worker):
        deadline = time.monotonic() + 5
        cadence = threading.Event()
        while time.monotonic() < deadline:
            try:
                port = json.loads(path.read_text(encoding='utf-8'))['port']
                if isinstance(port, int) and 1 <= port <= 65535:
                    return port
            except (OSError, ValueError, KeyError):
                pass
            if worker.done():
                raise RuntimeError(f'Interest child exited before readiness: {worker.result()}')
            cadence.wait(.01)
        raise RuntimeError('Interest child readiness watchdog')

    def check(condition, message):
        if not condition:
            raise RuntimeError(message)

    def scenario(pool, root, name):
        work = root / name
        work.mkdir()
        ready = work / 'server-ready.json'
        server = pool.submit(execute, name + '.server',
                             [str(probe), '--mode', 'server', '--scenario', name,
                              '--fixtures', str(fixtures), '--control', str(work), '--ready', str(ready)])
        workers, proxy = [], None
        try:
            port = readiness(ready, server)
            a_port = port
            if name == 'head-of-line':
                proxy_ready = work / 'proxy-ready.json'
                proxy = pool.submit(execute, name + '.proxy',
                                    [sys.executable, str(Path(__file__).resolve()), '--proxy-worker',
                                     '--upstream-port', str(port), '--ready', str(proxy_ready),
                                     '--control', str(work)])
                a_port = readiness(proxy_ready, proxy)
            for identity, selected_port in (('a', a_port), ('b', port)):
                workers.append(pool.submit(execute, name + '.client-' + identity,
                                            [str(probe), '--mode', 'client', '--scenario', name,
                                             '--fixtures', str(fixtures), '--control', str(work),
                                             '--identity', identity, '--port', str(selected_port)]))
        finally:
            # Every child is owned by process_runner, including readiness/error paths.
            clients = [worker.result(timeout=49) for worker in workers]
            server_row = server.result(timeout=49)
            proxy_row = proxy.result(timeout=49) if proxy else None
            group = [*clients, server_row, *([proxy_row] if proxy_row else [])]
            rows.extend(group)
        for row in group:
            check(row['status'] == 'PASS', f"{row['name']} failed: {row['stderr']} {row['stdout']}")
        observed = server_row['result']
        check(observed.get('connections') == (3 if name == 'policy-revocation' else 2), name + ': connection inventory')
        check(observed.get('candidates') == 128 and observed.get('visible_per_recipient') == 32, name + ': workload reduced')
        check(0 < observed.get('max_queue_bytes', 0) <= 16384
              and 0 < observed.get('max_queue_frames', 0) <= 4, name + ': queue bounds')
        check(0 < observed.get('max_selection_units', 0) <= 8192
              and observed.get('max_catalog_rows') == 128, name + ': selection work bounds')
        check(observed.get('max_policy_rows') == 128
              and 0 < observed.get('max_policy_scene_reads', 0) <= 128
              and 0 < observed.get('max_policy_comparisons', 0) <= 4096
              and 0 < observed.get('max_privacy_checks', 0) <= 32,
              name + ': unthrottled privacy/policy work bounds')
        check(observed.get('max_eligible_entities') == 63 and observed.get('max_omitted_entities') == 31,
              name + ': crowded selection was not measured')
        check(0 < observed.get('selection_call_samples', 0) <= observed.get('all_call_samples', 0) <= 100000
              and observed.get('selection_call_wall_p95_us', 0) > 0,
              name + ': actual selection wall-work distribution missing')
        check(observed.get('live_streams') == 0 and observed.get('live_sessions') == 0, name + ': leaked authority')
        for row in clients:
            client = row['result']
            check(client.get('visible') == 32 and client.get('privacy_checked')
                  and client.get('owned_results'), name + ': incomplete client convergence/privacy')
        if name == 'crowd-churn-budget':
            check(observed.get('source_tick') == 440 and observed.get('sample_count') == 20,
                  'Active20Hz schedule or20 measured socket windows incomplete')
            check(observed.get('pending_newer_source', 0) > 0, 'Source did not advance while publication awaited ACK')
            check(len(observed.get('recipients', [])) == 2, 'Missing recipient bandwidth evidence')
            durations = observed.get('sample_seconds', [])
            check(len(durations) == 20 and all(math.isfinite(value) and value > 0 for value in durations),
                  'Missing actual socket sample durations')
            for recipient in observed['recipients']:
                check(0 < recipient['join_down'] <= 32768 and 0 < recipient['join_up'] <= 32768,
                      'Join burst exceeded32KiB/direction')
                for direction, limit in (('down', 65536), ('up', 16384)):
                    samples = recipient[direction + '_bytes_per_second']
                    raw = recipient[direction + '_bytes']
                    check(len(samples) == 20 and len(raw) == 20
                          and all(math.isfinite(value) and value > 0 for value in samples),
                          'Incomplete/idle measured socket window')
                    check(all(math.isclose(value, count / duration, rel_tol=1e-9)
                              for value, count, duration in zip(samples, raw, durations)),
                          'Socket bytes were not normalized to measured interval duration')
                    nearest_rank = sorted(samples)[math.ceil(.95 * len(samples)) - 1]
                    check(nearest_rank <= limit and math.isclose(nearest_rank, recipient[direction + '_p95'], rel_tol=1e-5),
                          'Measured socket p95 exceeded target or used another percentile rule')
            for row in clients:
                check(row['result']['measured_updates'] >= 190 and row['result']['final_tick'] == 440,
                      'Bandwidth passed without useful32-actor active throughput')
            check(clients[0]['result'].get('warmup_ack_hold_ms') == 175,
                  'Missing explicit bounded warmup ACK fault')
        elif name == 'policy-revocation':
            check(observed.get('privacy_closures') == 1 and observed.get('revoked_queue_frames') == 1
                  and observed.get('privacy_bytes_unchanged'), 'Queued revocation did not fail closed')
            check(clients[0]['result'].get('reconnects') == 1 and clients[1]['result'].get('reconnects') == 0
                  and all(row['result']['final_tick'] == 2 for row in clients),
                  'Revoked recipient recovery or unaffected recipient convergence failed')
        else:
            check(300 <= observed.get('hol_completion_ms', 0) <= 3000
                  and observed.get('independent_peer_progress'), 'Actual head-of-line delay/progress gate failed')
            check(clients[0]['result'].get('control_after_state')
                  and all(row['result']['final_tick'] == 1 for row in clients), 'Control bypassed ordered state')
            check(all(row['result'].get('ready_reply_ms', 0) > 0 for row in clients)
                  and all(row.get('unshaped_state_ack_ms', 0) > 0 for row in observed['recipients']),
                  'Missing measured unshaped control round-trip/state-ACK reference')
            relay = proxy_row['result']
            check(relay.get('received_bytes') == relay.get('forwarded_bytes')
                  and relay.get('end_to_end_tls') and not relay.get('byte_corruption')
                  and relay.get('peak_buffer_bytes', 16385) <= 16384
                  and relay.get('rate_bytes_per_second') == 8192
                  and relay.get('shaped_bytes', 0) >= 3454 and relay.get('hold_ms', 0) >= 100,
                  'Relay did not preserve bounded end-to-end TLS byte stream')
            check(relay['shaped_bytes'] <= 8192 * max(0, relay['shaped_elapsed_ms'] - 100) / 1000 + 8,
                  'Measured relay bytes exceeded declared8KiB/s after hold')
        scenarios.append({'name': name, 'status': 'PASS', 'process_count': len(group),
                          'connections': observed['connections'], 'measurements': observed})

    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix='shark-interest-process-') as directory:
            with ThreadPoolExecutor(max_workers=4) as pool:
                for name in SCENARIOS:
                    scenario(pool, Path(directory), name)
        check(len(scenarios) == 3 and len(rows) == 10, 'Interest scenario/process inventory mismatch')
    except Exception as error:
        errors.append(str(error))
    fixture_hashes = {path.relative_to(fixtures).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in sorted(fixtures.rglob('*.pem'))}
    if len(fixture_hashes) != 17:
        errors.append('Public credential fixture inventory must retain17 PEM files')
    receipt = {
        'status': 'PASS' if not errors else 'FAIL',
        'scope': 'Two real enrolled local clients,128 interest candidates and32 actively changing visible affine actors per recipient. This per-connection checkpoint does not qualify LOCAL-MMO-1 full16-player/two-region/gameplay, WAN, animation or production capacity. Transparent relay delays intact TLS/TCP bytes, not packet-loss simulation.',
        'redaction': 'Public fixture paths/hashes, scheduling and aggregate counters only; no PEM contents, keys, session IDs or raw payloads.',
        'elapsed_seconds': time.monotonic() - started,
        'limits': {'clients': 2, 'candidates': 128, 'visible_per_recipient': 32,
                   'source_hz': 20, 'replication_hz': 10, 'warmup_seconds': 2,
                   'measured_seconds': 20, 'socket_down_p95_bytes_per_second': 65536,
                   'socket_up_p95_bytes_per_second': 16384, 'join_direction_bytes': 32768,
                   'framed_bytes_per_second': 49152, 'framed_burst_bytes': 8192,
                   'queue_frames': 4, 'queue_bytes': 16384, 'selection_work_units': 8192,
                   'percentile_rule': 'nearest-rank over20 complete elapsed-normalized one-second windows',
                   'minimum_measured_updates_per_client': 190, 'warmup_ack_hold_ms': 175,
                   'relay_hold_ms': 100, 'relay_bytes_per_second': 8192,
                   'relay_direction_buffer_bytes': 16384, 'hol_completion_ms': [300, 3000]},
        'fixture_sha256': fixture_hashes, 'scenarios': scenarios, 'processes': rows, 'errors': errors,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': receipt['status'], 'scenarios': len(scenarios),
                      'processes': len(rows), 'elapsed_seconds': receipt['elapsed_seconds'],
                      'output': str(args.output), 'errors': errors}, indent=2))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
