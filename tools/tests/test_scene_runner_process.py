"""Independent process checks for bounded, noninteractive scene-runner results."""
from pathlib import Path
import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
import json
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate result key')
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    runner = args.runner.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    records = []

    def call(name, options, expected, machine=True):
        process = run_process([str(runner), *options], cwd=output.parent, timeout=10)
        assert not process.timed_out and not process.cleanup_errors, name
        assert process.returncode == expected, (name, process.stdout, process.stderr)
        assert not process.stderr.strip(), (name, process.stderr)
        assert len(process.stdout.encode('utf-8')) <= 1024*1024, name
        records.append({'name': name, 'exit_code': process.returncode})
        if not machine:
            return process.stdout
        result = json.loads(process.stdout, object_pairs_hook=unique)
        assert result['format'] == 'shark.scene-run' and type(result['schema_version']) is int and result['schema_version'] == 1
        assert result['mode'] == ('none' if expected == 2 else 'cpu') and result['renderer_initialized'] is False
        assert result['status'] == ('PASS' if expected == 0 else 'FAIL' if expected == 1 else 'ERROR')
        return result

    try:
        assert 'Shark SceneRunner' in call('help', ['--help'], 0, False)
        assert 'schema 1' in call('version', ['--version'], 0, False)
        options = ['--seed','1337','--ticks','7','--hz','59','--run-id','process-test','--revision','test-revision',
                   '--input','0:offset:5','--input','3:offset:-7','--input','3:offset:9']
        first = call('deterministic-first', options, 0)
        second = call('deterministic-second', options, 0)
        # A new process gets an independent local incarnation. All semantic
        # observations, ordered inputs and explicit reproducibility context agree.
        def semantic(value):
            result = copy.deepcopy(value)
            for case in result['cases']:
                for key in ('initial_revision','final_revision'):
                    case[key].pop('instance')
            return result
        assert semantic(first) == semantic(second)
        assert first['seed'] == 1337 and first['tick_rate_hz'] == 59 and first['ticks'] == 7
        assert first['run_id'] == 'process-test' and first['revision'] == 'test-revision'
        assert first['random_algorithm'] == 'splitmix64-v1'
        assert first['engine_version'] == '0.1.0'
        assert first['inputs'] == [{'tick':0,'action':'offset','value':5}, {'tick':3,'action':'offset','value':-7}, {'tick':3,'action':'offset','value':9}]
        case = first['cases'][0]
        assert case['id'] == 'foundation' and case['status'] == 'PASS' and case['failure'] is None
        assert case['ticks_completed'] == 7 and case['assertions'] == 8 and case['random_draws'] == 7
        assert case['final_revision']['value'] == case['initial_revision']['value'] + 8
        with tempfile.TemporaryDirectory(prefix='shark-scene-runner-', dir=output.parent) as temporary:
            folder = Path(temporary)
            report_path = folder/'caf\u00e9 \u4e16\u754c.json'
            published = call('unicode-result', [*options,'--output',str(report_path)], 0)
            original = report_path.read_bytes()
            assert json.loads(original) == published
            assert published['artifacts'] == [{'role':'result','path':str(report_path),'format':'shark.scene-run'}]
            conflict = call('no-overwrite', [*options,'--output',str(report_path)], 2)
            assert conflict['error']['code'] == 'conflict' and report_path.read_bytes() == original
            assert conflict['error']['path'] == str(report_path)
            assert not list(folder.glob('.shark-run-stage-*'))
            failed_path = folder/'assertion.json'
            failed = call('intentional-scene-assertion', ['--case','intentional-failure','--output',str(failed_path)], 1)
            assert json.loads(failed_path.read_bytes()) == failed
            failure = failed['cases'][0]['failure']
            assert failure['kind'] == 'assertion' and failure['phase'] == 'tick' and failure['tick'] == 0
            assert failure['error']['operation'] == 'scene.run.assertion'
            assert failure['error']['entity_id'] == '10000000-0000-4000-8000-000000000004'
            assert failed['cases'][0]['ticks_completed'] == 0
            call('missing-parent', ['--output',str(folder/'absent'/'report.json')], 2)
            race_path = folder/'race.json'
            def contender(identifier):
                return run_process([str(runner),'--ticks','1','--run-id',identifier,'--output',str(race_path)],
                                   cwd=output.parent,timeout=10)
            with ThreadPoolExecutor(max_workers=2) as workers:
                racers = list(workers.map(contender, ['first-writer','second-writer']))
            assert sorted(p.returncode for p in racers) == [0,2]
            winner = None
            for index, process in enumerate(racers):
                assert not process.timed_out and not process.cleanup_errors and not process.stderr.strip()
                document = json.loads(process.stdout,object_pairs_hook=unique)
                assert document['status'] == ('PASS' if process.returncode == 0 else 'ERROR')
                if process.returncode == 0:
                    winner = document
                records.append({'name':'concurrent-writer-'+str(index),'exit_code':process.returncode})
            assert json.loads(race_path.read_bytes()) == winner
            linked = folder/'linked.json'
            try:
                linked.symlink_to(report_path)
            except OSError:
                records.append({'name':'symlink-result','status':'NOT_RUN','reason':'Host did not allow a test symlink'})
            else:
                call('symlink-result', ['--output',str(linked)], 2)
                assert report_path.read_bytes() == original
            assert not list(folder.glob('.shark-run-stage-*'))
        assert not Path(temporary).exists()
        for name, options in [
            ('gpu-unavailable',['--mode','gpu']),('zero-ticks',['--ticks','0']),
            ('too-many-ticks',['--ticks','10001']),('zero-rate',['--hz','0']),('high-rate',['--hz','1001']),
            ('numeric-overflow',['--seed','18446744073709551616']),('negative-seed',['--seed','-1']),
            ('duplicate-option',['--ticks','2','--ticks','3']),('unknown-case',['--case','FeatureWorld']),
            ('unknown-option',['--unknown','value']),('missing-value',['--ticks']),
            ('unordered-input',['--input','2:offset:1','--input','1:offset:1']),
            ('input-beyond-run',['--input','60:offset:1']),('unknown-action',['--input','0:grant:1']),
            ('bad-token',['--run-id','bad/id']),('input-overflow',['--input','0:offset:2147483648']),
            ('empty-output',['--output',''])]:
            rejected = call(name,options,2)
            assert rejected['error']['code'] and rejected['error']['operation'] and rejected['error']['message']
        maximum = call('maximum-seed',['--seed','18446744073709551615','--ticks','1','--hz','1000'],0)
        assert maximum['seed'] == 18446744073709551615
        result = {'status':'PASS','checks':len(records),'records':records,'managed_temporary_output_removed':True}
    except Exception as error:
        result = {'status':'FAIL','records':records,'error':str(error)}
    output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
