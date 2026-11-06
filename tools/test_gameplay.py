"""Run bounded, separate-process authoritative gameplay acceptance on loopback."""

from concurrent.futures import ThreadPoolExecutor
import argparse
import json
import math
from pathlib import Path
import shutil
import tempfile
import threading
import time

from process_runner import run_process


SCENARIOS = ('baseline', 'rtt100-loss1', 'rtt200-loss5')
CHILD_TIMEOUT_SECONDS = 60
READY_TIMEOUT_SECONDS = 5
MAXIMUM_CHILD_OUTPUT_BYTES = 2097152
MAXIMUM_SUMMARY_BYTES = 65536
MAXIMUM_DIAGNOSTIC_CHARACTERS = 16384


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def execute(name, command):
    started = time.monotonic()
    result = run_process(command, timeout=CHILD_TIMEOUT_SECONDS)
    output_bytes = len(result.stdout.encode('utf-8')) + len(result.stderr.encode('utf-8'))
    summary = None
    if output_bytes <= MAXIMUM_CHILD_OUTPUT_BYTES:
        try:
            lines = result.stdout.splitlines()
            if lines and len(lines[-1].encode('utf-8')) <= MAXIMUM_SUMMARY_BYTES:
                summary = json.loads(lines[-1])
        except json.JSONDecodeError:
            pass
    passed = (result.returncode == 0 and not result.timed_out and not result.cleanup_errors
              and isinstance(summary, dict) and summary.get('status') == 'PASS')
    row = {'name': name, 'status': 'PASS' if passed else 'FAIL',
           'elapsed_seconds': time.monotonic() - started,
           'exit_code': result.returncode, 'timed_out': result.timed_out,
           'cleanup_errors': list(result.cleanup_errors), 'output_bytes': output_bytes,
           'result': summary}
    if not passed or result.stderr:
        row['stdout'] = result.stdout[-MAXIMUM_DIAGNOSTIC_CHARACTERS:]
        row['stderr'] = result.stderr[-MAXIMUM_DIAGNOSTIC_CHARACTERS:]
    return row


def readiness(path, server):
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    cadence = threading.Event()
    while time.monotonic() < deadline:
        try:
            value = json.loads(path.read_text(encoding='utf-8'))
            port = value['port']
            if type(port) is int and 1 <= port <= 65535:
                return port
        except (OSError, ValueError, KeyError, TypeError):
            pass
        require(not server.done(), 'Gameplay server exited before readiness')
        # A real wait is confined to bounded child startup; gameplay owns its clock.
        cadence.wait(0.01)
    raise RuntimeError('Gameplay server readiness deadline expired')


def validate_scenario(name, rows):
    require(len(rows) == 3, name + ': expected one server and two client processes')
    by_role = {}
    for row in rows:
        require(row['status'] == 'PASS', row['name'] + ': child failed')
        result = row['result']
        require(result.get('scenario') == name, row['name'] + ': scenario differs')
        role = result.get('role')
        key = role if role == 'server' else 'client-' + str(result.get('identity'))
        require(key in ('server', 'client-a', 'client-b') and key not in by_role,
                row['name'] + ': invalid or duplicate child identity')
        require(row['name'] == name + '.' + key, row['name'] + ': child role differs')
        by_role[key] = result
    require(set(by_role) == {'server', 'client-a', 'client-b'},
            name + ': missing server or authenticated client')

    def count(result, field, minimum=0, maximum=2**64 - 1):
        value = result.get(field)
        require(type(value) is int and minimum <= value <= maximum,
                name + ': invalid ' + field + ' on ' + result['role'])
        return value

    def measurement(result, field, maximum):
        value = result.get(field)
        require(type(value) in (int, float) and math.isfinite(value) and 0 <= value <= maximum,
                name + ': invalid ' + field + ' on ' + result['role'])
        return value

    def pose(result, field, expected):
        value = result.get(field)
        require(type(value) in (int, float) and math.isfinite(value) and abs(value - expected) < 0.0001,
                name + ': ' + field + ' differs from the literal final pose')

    server = by_role['server']
    for field, expected in (('ticks', 320), ('authenticated', 3), ('reconnects', 1),
                            ('deaths', 1), ('respawns', 1), ('hits', 4), ('cooldowns', 1),
                            ('interactions', 1), ('replay_rejections', 3), ('grant_rejections', 1),
                            ('live_entities', 3), ('bodies', 3), ('final_a_health', 100),
                            ('final_b_health', 100), ('healthy_final_tick', 320), ('shaped_tick_limit', 319)):
        count(server, field, expected, expected)
    for field, minimum in (('pause_world_ticks', 10), ('disconnected_world_ticks', 10),
                           ('canonical_states', 320)):
        count(server, field, minimum)
    death_tick = count(server, 'death_tick', 1, 260)
    count(server, 'respawn_tick', death_tick + 60, death_tick + 60)
    require(1.25 <= measurement(server, 'wall_max_x', 1.6), name + ': native wall was not reached')
    delay_ms = {'baseline': 0, 'rtt100-loss1': 50, 'rtt200-loss5': 100}[name]
    count(server, 'request_delay_ms', delay_ms, delay_ms)
    pose(server, 'final_a_x', 0)
    pose(server, 'final_b_x', -2)
    for identity in ('a', 'b'):
        client = by_role['client-' + identity]
        second = identity == 'b'
        count(client, 'latest_tick', 320, 320)
        authenticated = 2 if second else 1
        count(client, 'authenticated', authenticated, authenticated)
        for field in ('session_replies', 'states_applied', 'replica_spawns'):
            count(client, field, 1)
        for field in ('reconnects', 'pauses'):
            expected = int(second)
            count(client, field, expected, expected)
        for field in ('duplicate_observations', 'stale_observations',
                      'loss_drops', 'duplicated_messages', 'inversions'):
            count(client, field)
        count(client, 'prediction_peak', int(not second), 8)
        count(client, 'replica_peak', 2, 2)
        count(client, 'replica_despawns', 1 if second else 2)
        count(client, 'life_changes', int(second), 320 if second else 0)
        count(client, 'grant_rejections', int(not second), int(not second))
        count(client, 'own_dead_states', int(second), 320 if second else 0)
        count(client, 'target_dead_states', int(not second), 0 if second else 320)
        count(client, 'observed_health_mask', 31, 31)
        require(client.get('observed_respawn') is True, name + ': client never observed the fresh respawn')
        minimum_effects = 0 if second else 6 if name == 'baseline' else 1
        count(client, 'effects', minimum_effects, 0 if second else 6)
        correction = measurement(client, 'maximum_correction_m', 1000000)
        latency = measurement(client, 'maximum_delivery_latency_ms', 60000)
        require(measurement(client, 'correction_p50_m', correction)
                <= measurement(client, 'correction_p95_m', correction), name + ': correction quantiles differ')
        require(measurement(client, 'delivery_latency_p50_ms', latency)
                <= measurement(client, 'delivery_latency_p95_ms', latency), name + ': latency quantiles differ')
        wall_correction = measurement(client, 'wall_correction_m', correction)
        if not second:
            require(wall_correction > 0.05, name + ': native wall did not cause an actual prediction correction')
            count(client, 'healthy_pause_ticks', 1)
            count(client, 'healthy_disconnected_ticks', 1)
        count(client, 'final_health', 100, 100)
        count(client, 'final_target_health', 100, 100)
        count(client, 'final_replica_entities', 2, 2)
        pose(client, 'final_x', -2 if second else 0)
        pose(client, 'final_y', 0)
        pose(client, 'final_z', 0)
        replica_x = client.get('final_replica_x')
        require(isinstance(replica_x, list) and len(replica_x) == 2, name + ': final replicas differ')
        for index, expected in enumerate((-2, 0)):
            pose({'replica_x': replica_x[index]}, 'replica_x', expected)
        shaped = name != 'baseline'
        expected_config = {
            'semantic_seed': 1 if name == 'rtt100-loss1' else 1337,
            'semantic_loss_per_10000': {'baseline': 0, 'rtt100-loss1': 100, 'rtt200-loss5': 500}[name],
            'semantic_duplication_per_10000': 5000 if shaped else 0,
            'semantic_reorder_per_10000': 3000 if shaped else 0,
            'semantic_reorder_delay_ms': 1500 if shaped else 10,
            'semantic_delay_ms': delay_ms,
        }
        for field, expected in expected_config.items():
            count(client, field, expected, expected)
        if shaped:
            for field in ('loss_drops', 'duplicated_messages', 'inversions'):
                count(client, field, 1)
        else:
            for field in ('loss_drops', 'duplicated_messages', 'inversions'):
                count(client, field, 0, 0)
    return {'name': name, 'status': 'PASS', 'processes': 3,
            'ticks': server['ticks'], 'authenticated_connections': server['authenticated'],
            'pause_world_ticks': server['pause_world_ticks'],
            'disconnected_world_ticks': server['disconnected_world_ticks']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    probe, fixtures = args.probe.resolve(), args.fixtures.resolve()
    rows, scenarios, errors, attempted = [], [], [], []
    work_root = None
    started = time.monotonic()
    try:
        require(probe.is_file() and fixtures.is_dir(), 'Gameplay probe or fixtures unavailable')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        work_root = Path(tempfile.mkdtemp(prefix='shark-gameplay-process-', dir=args.output.parent)).resolve()
        with ThreadPoolExecutor(max_workers=3) as pool:
            for name in SCENARIOS:
                attempted.append(name)
                initial_errors = len(errors)
                work = work_root / name
                work.mkdir()
                common = ['--scenario', name, '--fixtures', str(fixtures), '--control', str(work)]
                ready = work / 'server-ready.json'
                server = pool.submit(execute, name + '.server',
                                     [str(probe), '--mode', 'server', *common, '--ready', str(ready)])
                workers, group = [server], []
                try:
                    port = readiness(ready, server)
                    for identity in ('a', 'b'):
                        workers.append(pool.submit(
                            execute, name + '.client-' + identity,
                            [str(probe), '--mode', 'client', *common, '--port', str(port),
                             '--identity', identity]))
                except Exception as error:
                    errors.append(name + ': ' + str(error))
                finally:
                    # Collect every launched owner even if startup or another child fails.
                    for worker in workers:
                        try:
                            group.append(worker.result(timeout=CHILD_TIMEOUT_SECONDS + 5))
                        except Exception as error:
                            errors.append(name + ': child owner failed: ' + str(error))
                    rows.extend(group)
                try:
                    scenarios.append(validate_scenario(name, group))
                    print(json.dumps({'scenario': name, 'status': 'PASS', 'processes': 3}), flush=True)
                except Exception as error:
                    errors.append(str(error))
                if len(errors) != initial_errors:
                    # Preserve the first failing inputs instead of repeating a shared bug.
                    break
        require(len(scenarios) == 3 and len(rows) == 9, 'Gameplay scenario/process inventory incomplete')
        require(not errors, 'Gameplay failures retain their original work directory')
        require(work_root.parent == args.output.parent.resolve()
                and work_root.name.startswith('shark-gameplay-process-')
                and work_root.resolve() == work_root, 'Gameplay cleanup target left its owned directory')
        # Remove only this invocation's verified root after all children and checks pass.
        shutil.rmtree(work_root)
    except Exception as error:
        errors.append(str(error))
    receipt = {
        'schema_version': 1, 'status': 'PASS' if not errors else 'FAIL',
        'scope': ('Two authenticated headless client processes and one authoritative server per profile. '
                  'Real mTLS transport carries semantic request-admission delay and deterministic '
                  'observation delay, jitter, loss, reorder and duplication. These are semantic RTT '
                  'profiles, not TCP byte-loss or real WAN qualification. Client pause is headless.'),
        'elapsed_seconds': time.monotonic() - started,
        'limits': {'profiles': list(SCENARIOS), 'client_processes': 2, 'server_hz': 20,
                   'source_ticks': 320, 'child_timeout_seconds': CHILD_TIMEOUT_SECONDS,
                   'ready_timeout_seconds': READY_TIMEOUT_SECONDS,
                   'maximum_accepted_child_output_bytes': MAXIMUM_CHILD_OUTPUT_BYTES,
                   'maximum_summary_bytes': MAXIMUM_SUMMARY_BYTES,
                   'semantic_pending_packets': 64, 'semantic_pending_bytes': 65536},
        'scenarios': scenarios, 'processes': rows,
        'not_run_scenarios': [name for name in SCENARIOS if name not in attempted], 'errors': errors,
    }
    if errors and work_root is not None and work_root.exists():
        receipt['failure_work_directory'] = str(work_root)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': receipt['status'], 'scenarios': len(scenarios), 'processes': len(rows),
                      'output': str(args.output), 'errors': errors}), flush=True)
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
