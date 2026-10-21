"""Qualify integrated local network adversity with owned processes and bounded relays."""

from collections import deque
from concurrent.futures import ThreadPoolExecutor
import argparse
import ctypes
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import random
import select
import socket
import sys
import tempfile
import threading
import time

from process_runner import run_process


SCENARIOS = ('baseline', 'rtt100-loss1', 'rtt200-loss5', 'partition-reconnect', 'slow-reader', 'hostile')
RELAY_SCENARIOS = {'rtt100-loss1': 50, 'rtt200-loss5': 100, 'partition-reconnect': 0}
LIMITS = {
    'clients': 2, 'source_hz': 20, 'source_ticks': {name: (320 if name.startswith('rtt') else 120) for name in SCENARIOS}, 'seed': 1337,
    'candidates': 66, 'visible_per_recipient': 32,
    'minimum_healthy_updates': {name: (80 if name.startswith('rtt') else 30) for name in SCENARIOS},
    'minimum_baseline_a_updates': 30,
    'transport_queue_bytes': 16384, 'transport_queue_frames': 4,
    'client_storage_bytes': 32768, 'interest_storage_bytes': 262144,
    'replication_pending_updates': 2, 'replication_pending_records': 128,
    'client_history_records': 64, 'selection_work_units': 8192, 'policy_work_units': 4384,
    'semantic_packet_limit': 16, 'semantic_byte_limit': 65536,
    'semantic_max_packet_bytes': 4096, 'semantic_max_drain_count': 8,
    'semantic_reorder_delay_ms': 1500, 'semantic_jitter_ms': 30,
    'semantic_profile_seeds': {'rtt100-loss1': 1, 'rtt200-loss5': 1337},
    'semantic_profile_loss_per_10000': {'rtt100-loss1': 100, 'rtt200-loss5': 500},
    'semantic_duplication_per_10000': 5000, 'semantic_reorder_per_10000': 3000,
    'child_timeout_seconds': 60, 'ready_timeout_seconds': 5,
    'child_rss_bytes': 1073741824, 'child_handles_or_fds': 512,
    'resource_sample_interval_ms': 10, 'maximum_resource_samples': 6001,
    'relay_direction_buffer_bytes': 16384, 'relay_chunk_bytes': 4096,
    'relay_jitter_ms': [0, 30], 'relay_partition_ms': 500,
    'maximum_relay_chunks': 10000, 'maximum_child_output_bytes': 2097152,
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def strict_json(text, cap=2097152):
    require(len(text.encode('utf-8')) <= cap, 'JSON input exceeds the declared bound')

    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate JSON key')
            result[key] = value
        return result

    def constant(value):
        raise ValueError('Nonfinite JSON value: ' + value)

    result = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)

    def check(value, depth=0):
        require(depth <= 20, 'JSON nesting exceeds the declared bound')
        if isinstance(value, float):
            require(math.isfinite(value), 'Nonfinite JSON number')
        elif isinstance(value, dict):
            require(len(value) <= 256, 'JSON object field count exceeds the declared bound')
            for item in value.values():
                check(item, depth + 1)
        elif isinstance(value, list):
            require(len(value) <= 10000, 'JSON array exceeds the declared bound')
            for item in value:
                check(item, depth + 1)
        elif isinstance(value, str):
            require(len(value) <= 65536, 'JSON string exceeds the declared bound')
    check(result)
    return result


class ResourceMonitor:
    """Sample only the PID published by this invocation's owned child."""

    def __init__(self, pid_file):
        self.pid_file = pid_file
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name='owned-network-resource-monitor')
        self.samples = []
        self.errors = []
        self.pid = None
        self.end_sample_index = None

    def _run(self):
        handle = None
        try:
            deadline = time.monotonic() + LIMITS['ready_timeout_seconds']
            while not self.stop.is_set():
                try:
                    value = self.pid_file.read_text(encoding='utf-8').strip()
                    if not value:
                        require(time.monotonic() < deadline, 'Child PID file remained empty')
                        self.stop.wait(.005)
                        continue
                    require(value.isdecimal() and len(value) <= 10, 'Invalid child PID file')
                    self.pid = int(value)
                    require(self.pid > 1, 'Invalid child PID')
                    break
                except FileNotFoundError:
                    require(time.monotonic() < deadline, 'Child PID readiness timed out')
                    self.stop.wait(.005)
            if self.pid is None:
                return
            if os.name == 'nt':
                from ctypes import wintypes

                class MemoryCounters(ctypes.Structure):
                    _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD),
                                ('PeakWorkingSetSize', ctypes.c_size_t), ('WorkingSetSize', ctypes.c_size_t),
                                ('QuotaPeakPagedPoolUsage', ctypes.c_size_t), ('QuotaPagedPoolUsage', ctypes.c_size_t),
                                ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t), ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
                                ('PagefileUsage', ctypes.c_size_t), ('PeakPagefileUsage', ctypes.c_size_t)]

                kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
                kernel.OpenProcess.restype = wintypes.HANDLE
                kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
                kernel.CloseHandle.restype = wintypes.BOOL
                kernel.GetProcessHandleCount.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
                kernel.GetProcessHandleCount.restype = wintypes.BOOL
                kernel.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
                kernel.GetExitCodeProcess.restype = wintypes.BOOL
                kernel.K32GetProcessMemoryInfo.argtypes = (wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD)
                kernel.K32GetProcessMemoryInfo.restype = wintypes.BOOL
                handle = kernel.OpenProcess(0x0400 | 0x0010, False, self.pid)
                require(bool(handle), 'Cannot open owned child for resource sampling')

                def sample():
                    exit_code = wintypes.DWORD()
                    require(kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)), 'Cannot query owned child lifetime')
                    if exit_code.value != 259:
                        raise ProcessLookupError('Owned child has exited')
                    counters = MemoryCounters()
                    counters.cb = ctypes.sizeof(counters)
                    handles = wintypes.DWORD()
                    if not kernel.K32GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                        raise ProcessLookupError('Owned child is no longer readable')
                    if not kernel.GetProcessHandleCount(handle, ctypes.byref(handles)):
                        raise ProcessLookupError('Owned child handle inventory unavailable')
                    if not counters.WorkingSetSize or not handles.value:
                        require(kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)), 'Cannot query owned child lifetime')
                        if exit_code.value != 259:
                            raise ProcessLookupError('Owned child exited during resource sampling')
                    return int(counters.WorkingSetSize), int(handles.value)
            else:
                process = Path('/proc') / str(self.pid)
                identity = (process / 'stat').read_text().rsplit(')', 1)[1].split()[19]

                def sample():
                    fields = (process / 'stat').read_text().rsplit(')', 1)[1].split()
                    require(fields[19] == identity, 'Owned child PID identity changed')
                    values = dict(line.split(':', 1) for line in (process / 'status').read_text().splitlines() if ':' in line)
                    if 'VmRSS' not in values:
                        raise ProcessLookupError('Owned child has exited')
                    rss = int(values['VmRSS'].split()[0]) * 1024
                    descriptors = sum(1 for _ in itertools.islice((process / 'fd').iterdir(),
                                                                 LIMITS['child_handles_or_fds'] + 1))
                    return rss, descriptors

            started = time.monotonic()
            while not self.stop.is_set():
                try:
                    rss, descriptors = sample()
                except (FileNotFoundError, ProcessLookupError):
                    break
                require(rss > 0 and descriptors > 0, 'Empty live child resource sample')
                require(len(self.samples) < LIMITS['maximum_resource_samples'], 'Resource sample capacity exhausted')
                self.samples.append({'elapsed_ms': (time.monotonic() - started) * 1000,
                                     'rss_bytes': rss, 'handles_or_fds': descriptors})
                require(rss <= LIMITS['child_rss_bytes'], 'Owned child RSS bound exceeded')
                require(descriptors <= LIMITS['child_handles_or_fds'], 'Owned child descriptor bound exceeded')
                if len(self.samples) == 1:
                    self.pid_file.with_name(self.pid_file.name + '.observed').write_text('observed\n', encoding='utf-8')
                if (self.end_sample_index is None
                        and self.pid_file.with_name(self.pid_file.name + '.finish').exists()):
                    self.end_sample_index = len(self.samples) - 1
                    self.pid_file.with_name(self.pid_file.name + '.finished').write_text('observed\n', encoding='utf-8')
                    break
                self.stop.wait(LIMITS['resource_sample_interval_ms'] / 1000)
        except Exception as error:
            self.errors.append(str(error))
        finally:
            if handle:
                kernel.CloseHandle(handle)

    def start(self):
        self.thread.start()

    def finish(self):
        self.stop.set()
        self.thread.join(timeout=2)
        require(not self.thread.is_alive(), 'Resource monitor did not stop')
        require(self.samples and not self.errors, 'Owned child resource measurement failed: ' + '; '.join(self.errors))
        require(self.end_sample_index is not None, 'Child did not expose its cleanup resource checkpoint')
        first, last = self.samples[0], self.samples[self.end_sample_index]
        return {'status': 'PASS', 'pid': self.pid,
                'metric': 'Windows working-set RSS and process handles' if os.name == 'nt' else 'Linux VmRSS and /proc/<owned-pid>/fd entries',
                'sampling_scope': 'First observed live value and acknowledged post-cleanup live checkpoint; sampled peak, not an allocator or lifetime leak proof',
                'sample_count': len(self.samples), 'baseline': first, 'end': last,
                'end_sample_index': self.end_sample_index, 'last_live': self.samples[-1],
                'startup_acknowledged': True, 'cleanup_acknowledged': True,
                'peak_rss_bytes': max(row['rss_bytes'] for row in self.samples),
                'peak_handles_or_fds': max(row['handles_or_fds'] for row in self.samples),
                'rss_growth_bytes': last['rss_bytes'] - first['rss_bytes'],
                'handle_or_fd_growth': last['handles_or_fds'] - first['handles_or_fds'],
                'samples': self.samples, 'errors': []}


def publish_pid(path):
    path.write_text(str(os.getpid()) + '\n', encoding='utf-8')
    deadline = time.monotonic() + 3
    while not path.with_name(path.name + '.observed').exists():
        require(time.monotonic() < deadline, 'Resource observer did not acknowledge relay startup')
        threading.Event().wait(.005)


def finish_observation(path):
    path.with_name(path.name + '.finish').write_text('finished\n', encoding='utf-8')
    deadline = time.monotonic() + 3
    while not path.with_name(path.name + '.finished').exists():
        require(time.monotonic() < deadline, 'Resource observer did not acknowledge relay cleanup')
        threading.Event().wait(.005)


def relay_worker(args):
    """Preserve ordered TLS bytes; apply delivery delay or explicit disconnection."""
    publish_pid(args.pid_file)
    deadline = time.monotonic() + 55
    cap = LIMITS['relay_direction_buffer_bytes']
    rng = random.Random(LIMITS['seed'])
    received, forwarded, discarded, peaks = [0, 0], [0, 0], [0, 0], [0, 0]
    delay_samples = [[], []]
    scheduled_samples = [[], []]
    armed, partition_started, partition_dropped = False, None, False
    partition_elapsed_ms = 0
    connections = 0
    chunk_count = 0
    base_delay = RELAY_SCENARIOS[args.scenario]
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(2)
        listener.settimeout(.05)
        args.ready.write_text(json.dumps({'port': listener.getsockname()[1]}) + '\n', encoding='utf-8')
        expected = 2 if args.scenario == 'partition-reconnect' else 1
        while connections < expected:
            require(time.monotonic() < deadline, 'Relay connection watchdog expired')
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            connections += 1
            dropped_this_connection = False
            with client, socket.create_connection(('127.0.0.1', args.upstream_port), timeout=5) as upstream:
                sources, destinations = (client, upstream), (upstream, client)
                for connection in sources:
                    connection.setblocking(False)
                queues = [deque(), deque()]
                buffered, eof, half_closed = [0, 0], [False, False], [False, False]
                while time.monotonic() < deadline:
                    now = time.monotonic()
                    if not armed and (args.control / 'relay-arm').exists() and not any(buffered):
                        armed = True
                        (args.control / 'relay-armed').write_text('armed\n', encoding='utf-8')
                    if (args.scenario == 'partition-reconnect' and not partition_dropped
                            and partition_started is None and (args.control / 'partition-start').exists()):
                        require(armed, 'Partition began before relay priming')
                        partition_started = now
                        (args.control / 'partition-active').write_text('active\n', encoding='utf-8')
                    partitioned = partition_started is not None and not partition_dropped
                    if partitioned and (now - partition_started) * 1000 >= LIMITS['relay_partition_ms']:
                        for n in range(2):
                            discarded[n] += buffered[n]
                        partition_elapsed_ms = (now - partition_started) * 1000
                        partition_dropped = True
                        dropped_this_connection = True
                        break
                    reading = [sources[n] for n in range(2) if not eof[n] and buffered[n] < cap]
                    writing = [destinations[n] for n in range(2) if queues[n] and not partitioned
                               and queues[n][0][1] <= now]
                    readable, writable, _ = select.select(reading, writing, [], .002)
                    for n in range(2):
                        if sources[n] in readable:
                            try:
                                data = sources[n].recv(min(LIMITS['relay_chunk_bytes'], cap - buffered[n]))
                            except BlockingIOError:
                                data = None
                            except (ConnectionResetError, ConnectionAbortedError):
                                data = b''
                            if data is not None:
                                if not data:
                                    eof[n] = True
                                else:
                                    begun = time.monotonic()
                                    shaped = armed and base_delay > 0
                                    delay = base_delay + rng.randint(0, 30) if shaped else 0
                                    due = begun + delay / 1000
                                    if queues[n]:
                                        due = max(due, queues[n][-1][1])
                                    queues[n].append([bytearray(data), due, begun, shaped])
                                    buffered[n] += len(data)
                                    received[n] += len(data)
                                    peaks[n] = max(peaks[n], buffered[n])
                                    chunk_count += 1
                                    require(chunk_count <= LIMITS['maximum_relay_chunks'], 'Relay chunk budget exhausted')
                                    if shaped:
                                        scheduled_samples[n].append(delay)
                        if destinations[n] in writable and queues[n]:
                            head = queues[n][0]
                            try:
                                sent = destinations[n].send(head[0])
                            except BlockingIOError:
                                sent = 0
                            if sent:
                                del head[0][:sent]
                                buffered[n] -= sent
                                forwarded[n] += sent
                                if not head[0]:
                                    if head[3]:
                                        delay_samples[n].append((time.monotonic() - head[2]) * 1000)
                                    queues[n].popleft()
                        if eof[n] and not queues[n] and not half_closed[n]:
                            try:
                                destinations[n].shutdown(socket.SHUT_WR)
                            except OSError:
                                pass
                            half_closed[n] = True
                    if all(eof) and not any(buffered):
                        break
                else:
                    raise RuntimeError('Relay delivery watchdog expired')
            if dropped_this_connection:
                # Both actual TCP endpoints are closed before the reconnect barrier.
                (args.control / 'partition-dropped').write_text('dropped\n', encoding='utf-8')
    require(armed and received == [forwarded[n] + discarded[n] for n in range(2)],
            'Relay byte accounting or priming failed')
    if base_delay:
        require(all(delay_samples) and all(scheduled_samples), 'Relay did not shape both directions')
        require(all(value >= base_delay for samples in delay_samples for value in samples), 'Relay delivered before base delay')
    else:
        require(partition_dropped and connections == 2, 'Actual partition and reconnection missing')
    result = {'status': 'PASS', 'role': 'relay', 'scenario': args.scenario,
              'connections': connections, 'seed': LIMITS['seed'], 'base_one_way_ms': base_delay,
              'jitter_range_ms': LIMITS['relay_jitter_ms'], 'buffer_cap_bytes': cap,
              'received_bytes': received, 'forwarded_bytes': forwarded, 'discarded_bytes': discarded,
              'peak_buffer_bytes': peaks, 'chunks': chunk_count,
              'scheduled_delays_ms': scheduled_samples, 'observed_delays_ms': delay_samples,
              'partition_ms': LIMITS['relay_partition_ms'] if partition_dropped else 0,
              'partition_elapsed_ms': partition_elapsed_ms,
              'partition_dropped': partition_dropped, 'end_to_end_tls': True,
              'byte_corruption': False, 'tcp_packet_loss_emulation': False}
    finish_observation(args.pid_file)
    print(json.dumps(result), flush=True)
    return 0


def execute(name, command, pid_file):
    begun = time.monotonic()
    monitor = ResourceMonitor(pid_file)
    monitor.start()
    errors = []
    row = {'name': name, 'command': list(map(str, command)), 'status': 'FAIL',
           'exit_code': None, 'timed_out': False, 'cleanup_errors': [], 'stdout': '', 'stderr': ''}
    try:
        result = run_process(command, timeout=LIMITS['child_timeout_seconds'])
        row.update(exit_code=result.returncode, timed_out=result.timed_out,
                   cleanup_errors=result.cleanup_errors, stdout=result.stdout, stderr=result.stderr)
        require(len(result.stdout.encode('utf-8')) + len(result.stderr.encode('utf-8'))
                <= LIMITS['maximum_child_output_bytes'], 'Child output bound exceeded')
        lines = result.stdout.splitlines()
        require(bool(lines), 'Missing child result')
        row['result'] = strict_json(lines[-1])
        require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors
                and not result.stderr, 'Child failed or emitted diagnostics')
        require(isinstance(row['result'], dict) and row['result'].get('status') == 'PASS', 'Child result failed')
    except Exception as error:
        errors.append(str(error))
    finally:
        try:
            row['resources'] = monitor.finish()
        except Exception as error:
            errors.append(str(error))
        row['elapsed_seconds'] = time.monotonic() - begun
    row['errors'] = errors
    row['status'] = 'FAIL' if errors else 'PASS'
    return row


def readiness(path, worker):
    deadline = time.monotonic() + LIMITS['ready_timeout_seconds']
    while time.monotonic() < deadline:
        try:
            value = strict_json(path.read_text(encoding='utf-8'), 256)
            require(isinstance(value, dict) and set(value) == {'port'}, 'Unexpected readiness schema')
            port = value['port']
            require(type(port) is int and 1 <= port <= 65535, 'Invalid readiness port')
            return port
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        require(not worker.done(), 'Child exited before readiness: ' + str(worker.result() if worker.done() else ''))
        threading.Event().wait(.005)
    raise RuntimeError('Child readiness watchdog expired')


def validate_scenario(name, group):
    """Require observed events, exact role inventory and predeclared resource bounds."""
    def integer(value, minimum=0, maximum=2**64 - 1):
        return type(value) is int and minimum <= value <= maximum

    def number(value, minimum=0, maximum=60000):
        return type(value) in (int, float) and math.isfinite(value) and minimum <= value <= maximum

    def bounded(record, field, minimum, maximum):
        require(integer(record.get(field), minimum, maximum), name + ': invalid ' + field)

    def schema(record, fields):
        require(set(record) == set(fields.split()), name + ': unexpected result fields: '
                + str(sorted(set(record) ^ set(fields.split()))))

    def resources(record):
        schema(record, '''status pid metric sampling_scope sample_count baseline end end_sample_index
            last_live startup_acknowledged cleanup_acknowledged peak_rss_bytes peak_handles_or_fds
            rss_growth_bytes handle_or_fd_growth samples errors''')
        require(record['status'] == 'PASS' and record['errors'] == []
                and record['startup_acknowledged'] is True and record['cleanup_acknowledged'] is True,
                'Incomplete owned-process resource observation')
        bounded(record, 'pid', 2, 2**32 - 1)
        samples = record['samples']
        require(isinstance(samples, list) and 2 <= len(samples) <= LIMITS['maximum_resource_samples'], 'Incomplete resource samples')
        require(integer(record['sample_count']) and record['sample_count'] == len(samples), 'Resource sample count differs')
        previous = -1
        for sample in samples:
            schema(sample, 'elapsed_ms rss_bytes handles_or_fds')
            require(number(sample['elapsed_ms']) and sample['elapsed_ms'] >= previous, 'Invalid resource sample time')
            previous = sample['elapsed_ms']
            bounded(sample, 'rss_bytes', 1, LIMITS['child_rss_bytes'])
            bounded(sample, 'handles_or_fds', 1, LIMITS['child_handles_or_fds'])
        require(integer(record['end_sample_index']) and record['end_sample_index'] == len(samples) - 1,
                'Cleanup resource checkpoint is not the final sample')
        require(record['baseline'] == samples[0] and record['end'] == record['last_live'] == samples[-1],
                'Resource endpoints differ from raw samples')
        for field, sample_field in (('peak_rss_bytes', 'rss_bytes'), ('peak_handles_or_fds', 'handles_or_fds')):
            require(integer(record[field]) and record[field] == max(sample[sample_field] for sample in samples), 'Resource peak differs from raw samples')
        for field, sample_field in (('rss_growth_bytes', 'rss_bytes'), ('handle_or_fd_growth', 'handles_or_fds')):
            require(type(record[field]) is int and record[field] == samples[-1][sample_field] - samples[0][sample_field], 'Resource growth differs from raw samples')

    required = {name + '.server', name + '.client-a', name + '.client-b'}
    if name in RELAY_SCENARIOS:
        required.add(name + '.relay')
    require({row['name'] for row in group} == required and len(group) == len(required), 'Scenario process inventory differs')
    for row in group:
        require(row['status'] == 'PASS', row['name'] + ': ' + '; '.join(row['errors']))
        require(type(row.get('exit_code')) is int and row['exit_code'] == 0
                and row.get('timed_out') is False and not row.get('cleanup_errors')
                and not row.get('stderr') and row.get('errors') == [], 'Child exit/cleanup failed')
        require(number(row.get('elapsed_seconds'), .001, 65), 'Child elapsed duration exceeds the declared bound')
        result = row['result']
        require(result == strict_json(row['stdout'].splitlines()[-1]) and result.get('status') == 'PASS', 'Child result does not match captured JSON')
        role = row['name'].split('.')[-1]
        require(result.get('scenario') == name, 'Child scenario mismatch')
        require(result.get('role') == ('client' if role.startswith('client-') else role), 'Child role mismatch')
        if role.startswith('client-'):
            require(result.get('identity') == role[-1], 'Client identity mismatch')
        resources(row['resources'])
    by_role = {row['name'].split('.')[-1]: row['result'] for row in group}
    server = by_role['server']
    schema(server, '''status role scenario source_tick connections candidates visible_per_recipient
        accepted_controls rejected_controls malformed_controls live_streams live_sessions max_queue_bytes
        max_queue_frames max_interest_storage_bytes interest_storage_limit_bytes max_replication_pending_updates
        max_replication_pending_records max_policy_units max_selection_units independent_peer_progress
        healthy_updates_during_fault slow_timeouts uncertain_disconnects final_cleanup''')
    bounded(server, 'source_tick', LIMITS['source_ticks'][name], LIMITS['source_ticks'][name])
    expected_connections = 3 if name in ('partition-reconnect', 'slow-reader') else 2
    bounded(server, 'connections', expected_connections, expected_connections)
    bounded(server, 'candidates', 66, 66)
    bounded(server, 'visible_per_recipient', 32, 32)
    bounded(server, 'accepted_controls', 3, 10000)
    for field in ('rejected_controls', 'malformed_controls', 'healthy_updates_during_fault', 'slow_timeouts', 'uncertain_disconnects'):
        bounded(server, field, 0, 10000)
    for field in ('live_streams', 'live_sessions'):
        bounded(server, field, 0, 0)
    for field, cap in (('max_queue_bytes', LIMITS['transport_queue_bytes']), ('max_queue_frames', LIMITS['transport_queue_frames']),
                       ('max_interest_storage_bytes', LIMITS['interest_storage_bytes']),
                       ('max_replication_pending_updates', LIMITS['replication_pending_updates']),
                       ('max_replication_pending_records', LIMITS['replication_pending_records']),
                       ('max_policy_units', LIMITS['policy_work_units']), ('max_selection_units', LIMITS['selection_work_units'])):
        bounded(server, field, 1, cap)
    bounded(server, 'interest_storage_limit_bytes', LIMITS['interest_storage_bytes'], LIMITS['interest_storage_bytes'])
    require(server['final_cleanup'] is True and type(server['independent_peer_progress']) is bool, 'Server cleanup/progress evidence invalid')
    require(server['independent_peer_progress'] == (server['healthy_updates_during_fault'] > 0), 'Healthy peer progress counters disagree')
    if name in ('partition-reconnect', 'slow-reader', 'hostile'):
        require(server['independent_peer_progress'] and server['healthy_updates_during_fault'] > 0, 'Other recipient did not progress during adversity')
    require(server['slow_timeouts'] == (1 if name == 'slow-reader' else 0), 'Slow-reader timeout not observed exactly once')
    require(server['uncertain_disconnects'] == (1 if name == 'partition-reconnect' else 0), 'Uncertain admission was not recovered exactly once')
    require(server['malformed_controls'] == (1 if name == 'hostile' else 0), 'Malformed control inventory differs')
    if name == 'hostile':
        require(server['rejected_controls'] == 2, 'Hostile ownership/replay controls did not reject')

    for identity in ('a', 'b'):
        client = by_role['client-' + identity]
        schema(client, '''status role scenario identity source_tick applied duplicates stale_rejections resyncs
            reconnects expected_rejections privacy_checked final_entities max_queue_bytes max_queue_frames
            max_client_storage_bytes client_storage_limit_bytes max_client_history_records queued_fault_packets_peak
            queued_fault_bytes_peak loss_drops duplicate_messages reordered_messages delivered_reorder_inversions
            delivered_copies semantic_submissions semantic_faults_enabled semantic_seed semantic_loss_per_10000
            semantic_duplication_per_10000 semantic_reorder_per_10000 semantic_jitter_ms semantic_reorder_delay_ms
            semantic_packet_limit semantic_byte_limit semantic_max_packet_bytes semantic_max_drain_count
            semantic_replay_verified final_cleanup ack_latency_ms''')
        bounded(client, 'source_tick', LIMITS['source_ticks'][name], LIMITS['source_ticks'][name])
        minimum = LIMITS['minimum_healthy_updates'][name] if identity == 'b' else (30 if name == 'baseline' else 3)
        bounded(client, 'applied', minimum, 10000)
        for field in ('duplicates', 'stale_rejections', 'resyncs', 'expected_rejections', 'loss_drops',
                      'duplicate_messages', 'reordered_messages', 'delivered_reorder_inversions', 'delivered_copies', 'semantic_submissions'):
            bounded(client, field, 0, 10000)
        reconnects = 1 if identity == 'a' and name in ('partition-reconnect', 'slow-reader') else 0
        bounded(client, 'reconnects', reconnects, reconnects)
        expected_rejections = 3 if identity == 'a' and name == 'hostile' else reconnects
        bounded(client, 'expected_rejections', expected_rejections, expected_rejections)
        bounded(client, 'final_entities', 32, 32)
        require(client['privacy_checked'] is True and client['final_cleanup'] is True
                and client['semantic_replay_verified'] is True, 'Client privacy/cleanup/replay verification missing')
        for field, cap in (('max_queue_bytes', 16384), ('max_queue_frames', 4),
                           ('max_client_storage_bytes', 32768), ('max_client_history_records', 64)):
            bounded(client, field, 1, cap)
        bounded(client, 'client_storage_limit_bytes', 32768, 32768)
        for field, cap in (('queued_fault_packets_peak', 16), ('queued_fault_bytes_peak', 65536)):
            bounded(client, field, 0, cap)
        enabled = identity == 'a' and name.startswith('rtt')
        require(client['semantic_faults_enabled'] is enabled, 'Semantic fault enablement differs')
        for field in ('semantic_packet_limit', 'semantic_byte_limit', 'semantic_max_packet_bytes', 'semantic_max_drain_count',
                      'semantic_reorder_delay_ms', 'semantic_jitter_ms', 'semantic_duplication_per_10000', 'semantic_reorder_per_10000'):
            bounded(client, field, LIMITS[field], LIMITS[field])
        bounded(client, 'semantic_seed', 1 if name == 'rtt100-loss1' else 1337, 1 if name == 'rtt100-loss1' else 1337)
        bounded(client, 'semantic_loss_per_10000', 100 if name == 'rtt100-loss1' else 500, 100 if name == 'rtt100-loss1' else 500)
        if enabled:
            for field in ('loss_drops', 'duplicate_messages', 'reordered_messages', 'delivered_reorder_inversions',
                          'resyncs', 'semantic_submissions', 'delivered_copies', 'queued_fault_packets_peak', 'queued_fault_bytes_peak'):
                require(client[field] > 0, 'Configured semantic fault had no observed event: ' + field)
            require(client['duplicates'] + client['stale_rejections'] > 0, 'No repeated/stale state was actually rejected')
            admitted = client['semantic_submissions'] - client['loss_drops']
            require(admitted > 0 and client['duplicate_messages'] <= admitted
                    and client['reordered_messages'] <= admitted
                    and client['delivered_copies'] == admitted + client['duplicate_messages'],
                    'Semantic loss/admission/copy accounting does not balance')
            require(client['delivered_reorder_inversions'] <= client['delivered_copies'],
                    'Semantic reordering observations exceed delivered copies')
        else:
            for field in ('loss_drops', 'duplicate_messages', 'reordered_messages', 'delivered_reorder_inversions',
                          'semantic_submissions', 'delivered_copies', 'queued_fault_packets_peak', 'queued_fault_bytes_peak'):
                bounded(client, field, 0, 0)
        latencies = client['ack_latency_ms']
        require(isinstance(latencies, list) and 1 <= len(latencies) <= 256
                and all(number(value, .000001, 55000) for value in latencies), 'ACK latency observations incomplete or invalid')

    if name in RELAY_SCENARIOS:
        relay = by_role['relay']
        schema(relay, '''status role scenario connections seed base_one_way_ms jitter_range_ms buffer_cap_bytes
            received_bytes forwarded_bytes discarded_bytes peak_buffer_bytes chunks scheduled_delays_ms
            observed_delays_ms partition_ms partition_elapsed_ms partition_dropped end_to_end_tls byte_corruption tcp_packet_loss_emulation''')
        bounded(relay, 'connections', 2 if name == 'partition-reconnect' else 1, 2 if name == 'partition-reconnect' else 1)
        bounded(relay, 'seed', 1337, 1337)
        bounded(relay, 'base_one_way_ms', RELAY_SCENARIOS[name], RELAY_SCENARIOS[name])
        bounded(relay, 'buffer_cap_bytes', 16384, 16384)
        bounded(relay, 'chunks', 1, LIMITS['maximum_relay_chunks'])
        bounded(relay, 'partition_ms', 500 if name == 'partition-reconnect' else 0,
                500 if name == 'partition-reconnect' else 0)
        require(number(relay['partition_elapsed_ms']), 'Invalid measured partition duration')
        require(relay['jitter_range_ms'] == [0, 30] and all(type(value) is int for value in relay['jitter_range_ms']), 'Relay jitter limits changed')
        require(relay['end_to_end_tls'] is True and relay['byte_corruption'] is False
                and relay['tcp_packet_loss_emulation'] is False, 'Relay fault-layer description is incorrect')
        for field in ('received_bytes', 'forwarded_bytes', 'discarded_bytes', 'peak_buffer_bytes'):
            require(isinstance(relay[field], list) and len(relay[field]) == 2
                    and all(integer(value, 0, 16384 if field == 'peak_buffer_bytes' else 2**32 - 1) for value in relay[field]), 'Invalid relay direction accounting')
        require(all(relay['received_bytes'][n] == relay['forwarded_bytes'][n] + relay['discarded_bytes'][n]
                    and relay['forwarded_bytes'][n] > 0 and relay['peak_buffer_bytes'][n] > 0 for n in range(2)), 'Relay byte accounting mismatch')
        for field in ('scheduled_delays_ms', 'observed_delays_ms'):
            require(isinstance(relay[field], list) and len(relay[field]) == 2, 'Relay direction timing missing')
        if name == 'partition-reconnect':
            require(relay['partition_dropped'] is True and relay['partition_ms'] == 500
                    and number(relay['partition_elapsed_ms'], 500, 3000), 'Actual partition duration/reconnection missing')
            require(relay['scheduled_delays_ms'] == relay['observed_delays_ms'] == [[], []], 'Partition unexpectedly used delay profile')
            require(sum(relay['discarded_bytes']) > 0, 'Partition did not withhold any actual bytes')
        else:
            require(relay['partition_dropped'] is False and relay['partition_ms'] == 0
                    and relay['partition_elapsed_ms'] == 0 and relay['discarded_bytes'] == [0, 0], 'Delay profile lost or partitioned transport bytes')
            base = RELAY_SCENARIOS[name]
            for scheduled, observed in zip(relay['scheduled_delays_ms'], relay['observed_delays_ms']):
                require(isinstance(scheduled, list) and isinstance(observed, list)
                        and 1 <= len(scheduled) == len(observed) <= LIMITS['maximum_relay_chunks'], 'Relay shaped samples missing')
                require(all(integer(value, base, base + 30) for value in scheduled)
                        and any(value > base for value in scheduled)
                        and all(number(value, base, 55000) for value in observed)
                        and all(actual >= requested for actual, requested in zip(observed, scheduled)),
                        'Relay schedule or measured delay outside declared bounds')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--relay-worker', action='store_true')
    parser.add_argument('--upstream-port', type=int)
    parser.add_argument('--ready', type=Path)
    parser.add_argument('--control', type=Path)
    parser.add_argument('--pid-file', type=Path)
    parser.add_argument('--scenario', choices=SCENARIOS)
    args = parser.parse_args()
    if args.relay_worker:
        try:
            require(args.scenario in RELAY_SCENARIOS and args.upstream_port and args.ready
                    and args.control and args.pid_file, 'Incomplete relay arguments')
            return relay_worker(args)
        except Exception as error:
            print(str(error), file=sys.stderr)
            print(json.dumps({'status': 'FAIL', 'role': 'relay', 'scenario': args.scenario, 'error': 'adversity-relay'}))
            return 1
    if args.probe is None or args.fixtures is None or args.output is None:
        parser.error('--probe, --fixtures and --output are required')
    probe, fixtures = args.probe.resolve(), args.fixtures.resolve()
    rows, scenarios, errors = [], [], []
    started = time.monotonic()
    try:
        require(probe.is_file() and fixtures.is_dir(), 'Probe or fixtures unavailable')
        with tempfile.TemporaryDirectory(prefix='shark-adversity-process-') as directory:
            task_root = Path(directory).resolve()
            require(task_root.parent == Path(tempfile.gettempdir()).resolve()
                    and task_root.name.startswith('shark-adversity-process-'),
                    'Temporary cleanup target is outside the owned task directory')
            with ThreadPoolExecutor(max_workers=4) as pool:
                for name in SCENARIOS:
                    work = task_root / name
                    work.mkdir()
                    common = ['--scenario', name, '--fixtures', str(fixtures), '--control', str(work)]
                    ready, pid = work / 'server-ready.json', work / 'server.pid'
                    server = pool.submit(execute, name + '.server',
                        [str(probe), '--mode', 'server', *common, '--ready', str(ready), '--pid-file', str(pid)], pid)
                    workers = [server]
                    try:
                        port = readiness(ready, server)
                        a_port = port
                        if name in RELAY_SCENARIOS:
                            relay_ready, relay_pid = work / 'relay-ready.json', work / 'relay.pid'
                            relay = pool.submit(execute, name + '.relay',
                                [sys.executable, str(Path(__file__).resolve()), '--relay-worker', '--scenario', name,
                                 '--upstream-port', str(port), '--ready', str(relay_ready),
                                 '--control', str(work), '--pid-file', str(relay_pid)], relay_pid)
                            workers.append(relay)
                            a_port = readiness(relay_ready, relay)
                        for identity, selected_port in (('a', a_port), ('b', port)):
                            pid = work / ('client-' + identity + '.pid')
                            workers.append(pool.submit(execute, name + '.client-' + identity,
                                [str(probe), '--mode', 'client', *common, '--identity', identity,
                                 '--port', str(selected_port), '--pid-file', str(pid)], pid))
                    finally:
                        group = [worker.result(timeout=65) for worker in workers]
                        rows.extend(group)
                    validate_scenario(name, group)
                    scenarios.append({'name': name, 'status': 'PASS', 'process_count': len(group),
                                      'measurements': next(row['result'] for row in group if row['name'].endswith('.server'))})
                    print(json.dumps({'scenario': name, 'status': 'PASS', 'processes': len(group)}), flush=True)
        require(len(scenarios) == 6 and len(rows) == 21, 'Integrated scenario/child inventory incomplete')
    except Exception as error:
        errors.append(str(error))
    fixture_hashes = {path.relative_to(fixtures).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in sorted(fixtures.rglob('*.pem'))}
    if len(fixture_hashes) != 17:
        errors.append('Expected the seventeen unchanged public PEM fixtures')
    receipt = {'schema_version': 1, 'status': 'PASS' if not errors else 'FAIL',
        'scope': 'Two authenticated local recipients under integrated semantic message faults and real ordered TLS delay/disconnection; short active fixture, not full LOCAL-MMO-1 or production capacity.',
        'resource_scope': 'Owned child process sampled RSS and handles/fds; bounded short-run observations, not allocator accounting, unsampled peaks or a lifetime leak proof.',
        'output_scope': 'The output byte limit rejects oversized captured results; process_runner owns file capture and cleanup, so this is not a live output-storage quota.',
        'elapsed_seconds': time.monotonic() - started, 'limits': LIMITS,
        'fixture_sha256': fixture_hashes, 'scenarios': scenarios, 'processes': rows, 'errors': errors}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': receipt['status'], 'scenarios': len(scenarios), 'processes': len(rows),
                      'elapsed_seconds': receipt['elapsed_seconds'], 'output': str(args.output), 'errors': errors}, indent=2))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
