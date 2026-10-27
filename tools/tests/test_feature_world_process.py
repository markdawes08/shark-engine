"""Exercise the saved FeatureWorld and independently resolve its coverage catalog."""
from pathlib import Path
import argparse
import copy
import hashlib
import json
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON key: ' + key)
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    records = []
    summary = {'status': 'FAIL', 'records': records}

    def call(name, options, expected, catalog=False):
        process = run_process([str(args.runner.resolve()), *options], cwd=output.parent, timeout=10)
        assert not process.timed_out and not process.cleanup_errors, name
        assert process.returncode == expected and not process.stderr.strip(), (name, process.stdout, process.stderr)
        assert len(process.stdout.encode('utf-8')) <= 1024 * 1024, name
        document = json.loads(process.stdout, object_pairs_hook=unique)
        assert type(document['schema_version']) is int and document['schema_version'] == 1
        if not catalog:
            assert document['format'] == 'shark.scene-run'
            assert document['status'] == {0: 'PASS', 1: 'FAIL', 2: 'ERROR'}[expected]
            assert document['mode'] == ('none' if expected == 2 else 'cpu')
            assert document['renderer_initialized'] is False
        records.append({'name': name, 'exit_code': process.returncode})
        return document

    def semantic(document):
        result = copy.deepcopy(document)
        for case in result['cases']:
            for field in ('initial_revision', 'final_revision'):
                case[field].pop('instance')
        return result

    try:
        catalog = call('native-catalog', ['--list-cases'], 0, True)
        assert catalog['format'] == 'shark.scene-cases' and catalog['mode'] == 'cpu'
        manifest = json.loads(args.manifest.read_text(encoding='utf-8'), object_pairs_hook=unique)
        case_ids = manifest['featureworld']['case_ids']
        assert catalog['suites'] == {'featureworld': case_ids}
        assert catalog['cases'] == ['foundation', 'intentional-failure', *case_ids]
        assert len(set(case_ids)) == 8
        mapped = []
        external = []
        for category in ('components', 'commands'):
            for entry in manifest['implemented_surface'][category]:
                if not entry.get('featureworld_cases'):
                    assert isinstance(entry.get('external_fixture_reason'), str) and entry['external_fixture_reason'].strip(), entry['id']
                    assert entry['fixtures'] and entry['test_sources'], entry['id']
                    external.append(entry['id'])
                    continue
                assert entry['featureworld_cases'] and set(entry['featureworld_cases']) <= set(case_ids), entry['id']
                assert 'shark.tools.featureworld-process' in entry['test_ids'], entry['id']
                mapped.append(entry['id'])
        assert manifest['implemented_surface']['lua_api'] == []
        summary['mapped_surfaces'] = mapped
        summary['external_surfaces'] = external
        source_paths = [Path('FeatureWorld.sharkproject'), Path('scenes/FeatureWorld.sharkscene')]
        original = {path.as_posix(): (args.fixture / path).read_bytes() for path in source_paths}
        summary['source_sha256'] = {path: hashlib.sha256(data).hexdigest() for path, data in original.items()}
        with tempfile.TemporaryDirectory(prefix='shark-featureworld-process-', dir=output.parent) as temporary:
            folder = Path(temporary)
            project = folder / 'caf\u00e9 \u4e16\u754c'
            (project / 'scenes').mkdir(parents=True)
            for path in source_paths:
                shutil.copyfile(args.fixture / path, project / path)
            scratch = folder / 'scratch'
            scratch.mkdir()
            project_file = project / 'FeatureWorld.sharkproject'
            scene_file = project / 'scenes/FeatureWorld.sharkscene'
            options = ['--case', 'featureworld', '--project', str(project_file), '--workspace', str(scratch),
                       '--ticks', '3', '--hz', '20', '--seed', '1337', '--revision', 'featureworld-process', '--run-id', 'saved']
            first = call('saved-suite', options, 0)
            second = call('saved-suite-repeat', options, 0)
            assert semantic(first) == semantic(second)
            assert first['project_id'] == '20000000-0000-4000-8000-000000000001'
            assert first['scene_id'] == '20000000-0000-4000-8000-000000000003'
            assert [case['id'] for case in first['cases']] == case_ids
            assert [case['final_revision']['value'] for case in first['cases']] == [4, 11, 7, 6, 4, 4, 4, 4]
            for case in first['cases']:
                assert case['status'] == 'PASS' and case['failure'] is None
                assert case['ticks_completed'] == 3 and case['assertions'] >= 10
                assert case['initial_revision']['value'] == 4
            assert first['cases'][4]['initial_revision']['instance'] != first['cases'][4]['final_revision']['instance']
            summary['cases'] = first['cases']
            artifact = folder / 'result.json'
            published = call('saved-suite-publication', [*options, '--output', str(artifact)], 0)
            assert json.loads(artifact.read_bytes(), object_pairs_hook=unique) == published
            for case_id in case_ids:
                selected = call(case_id, ['--case', case_id, '--project', str(project_file),
                                         '--workspace', str(scratch), '--ticks', '1'], 0)
                assert len(selected['cases']) == 1 and selected['cases'][0]['id'] == case_id
            assert not list(scratch.iterdir()), 'Case scratch projects leaked'
            for path in source_paths:
                assert (project / path).read_bytes() == original[path.as_posix()], 'Input fixture was modified'
            call('missing-project-option', ['--case', 'featureworld', '--workspace', str(scratch)], 2)
            call('missing-workspace-option', ['--case', 'featureworld', '--project', str(project_file)], 2)
            call('project-with-foundation', ['--project', str(project_file)], 2)
            call('workspace-with-foundation', ['--workspace', str(scratch)], 2)
            call('unknown-feature-case', ['--case', 'featureworld.future', '--project', str(project_file)], 2)
            call('feature-input-rejection', [*options, '--input', '0:offset:1'], 2)
            call('missing-project-file', [*options[:3], str(project / 'missing.sharkproject'), *options[4:]], 2)
            sentinel = folder / 'keep.txt'
            sentinel.write_text('keep', encoding='utf-8')
            bad_workspace = list(options)
            bad_workspace[bad_workspace.index('--workspace') + 1] = str(sentinel)
            failed = call('scratch-parent-is-file', bad_workspace, 1)
            assert failed['cases'][-1]['failure']['phase'] == 'tick' and sentinel.read_text() == 'keep'
            # A valid source edit must fail actual expectations, not merely parse.
            scene = json.loads(scene_file.read_text(encoding='utf-8'))
            scene['entities'][2]['components']['shark.name']['value'] = 'unexpected authored edit'
            scene_file.write_text(json.dumps(scene), encoding='utf-8')
            failed_artifact = folder / 'failure.json'
            failed = call('valid-source-drift', [*options, '--output', str(failed_artifact)], 1)
            assert json.loads(failed_artifact.read_bytes()) == failed
            assert all(case['failure']['phase'] == 'setup' and case['ticks_completed'] == 0 for case in failed['cases'])
            assert all(case['failure']['kind'] == 'assertion' for case in failed['cases'])
            # Invalid versions/IDs/paths fail before native cases are dispatched.
            variants = []
            scene = json.loads(original['scenes/FeatureWorld.sharkscene'])
            future = copy.deepcopy(scene); future['entities'][0]['components']['shark.name']['schema_version'] = 2
            variants.append(('future-component-version', future))
            duplicate = copy.deepcopy(scene); duplicate['entities'][1]['entity_id'] = duplicate['entities'][0]['entity_id']
            variants.append(('duplicate-source-id', duplicate))
            cycle = copy.deepcopy(scene); cycle['entities'][0]['parent'] = cycle['entities'][2]['entity_id']
            variants.append(('source-cycle', cycle))
            for name, variant in variants:
                scene_file.write_text(json.dumps(variant), encoding='utf-8')
                call(name, options, 2)
            scene_file.write_bytes(original['scenes/FeatureWorld.sharkscene'])
            project_doc = json.loads(original['FeatureWorld.sharkproject'])
            project_doc['scenes'][0]['path'] = '../outside.sharkscene'
            project_file.write_text(json.dumps(project_doc), encoding='utf-8')
            call('escaping-source-path', options, 2)
            project_file.write_bytes(original['FeatureWorld.sharkproject'])
            call('recovered-valid-source', options, 0)
            assert not list(scratch.iterdir()) and not list(folder.glob('.shark-run-stage-*'))
        for path in source_paths:
            assert (args.fixture / path).read_bytes() == original[path.as_posix()], 'Versioned input changed'
        summary['status'] = 'PASS'
    except Exception as error:
        summary['error'] = repr(error)
    summary['checks'] = len(records)
    output.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary))
    return 0 if summary['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
