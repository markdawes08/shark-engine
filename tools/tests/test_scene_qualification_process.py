"""Connected CPU coverage/result qualification; actual GPU runs remain explicit."""
from pathlib import Path
import argparse
import copy
import json
import re
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import scene_runner
import validate_contract
from process_runner import run_process

ROOT = scene_runner.ROOT
# Independently reviewed against public services and the native FeatureWorld
# callbacks. New surfaces must extend their actual exercises and this inventory.
SCENE_SURFACES = {
    'component.name', 'component.transform', 'component.camera', 'component.mesh-renderer',
    'authoring.camera.set', 'authoring.mesh-renderer.set', 'tools.scene-runner.catalog',
    'tools.scene-runner.run', 'tools.scene-runner.results',
    'authoring.mutation.status', 'authoring.mutation.execute', 'authoring.mutation.query',
    'authoring.entity.create', 'authoring.entity.delete', 'authoring.entity.reparent',
    'authoring.name.set', 'authoring.transform.set', 'authoring.batch.preview',
    'authoring.batch.execute', 'authoring.history.inspect', 'authoring.history.undo',
    'authoring.history.redo', 'authoring.history.clear',
    'asset.metadata.read', 'asset.metadata.write', 'asset.catalog.refresh',
    'asset.catalog.query', 'asset.catalog.resolve',
    'asset.importers', 'asset.import.prepare', 'asset.import.publish', 'asset.cache.query',
    'asset.cache.read', 'asset.cache.usage', 'asset.cache.reload', 'asset.cache.prune',
    'asset.cache.clear', 'asset.cache.recover',
    'asset.jobs.submit', 'asset.jobs.query', 'asset.jobs.cancel', 'asset.jobs.drain',
    'asset.jobs.take', 'asset.jobs.close',
    'asset.gltf.inspect', 'asset.gltf.convert', 'asset.gltf.textures',
    'asset.gltf.materials', 'asset.gltf.asset.convert', 'asset.gltf.asset.encode', 'asset.gltf.asset.decode',
    'content.mesh.prepare', 'content.scene.prepare', 'content.gltf-scene.prepare',
    'scene.render.extract', 'content.client-export',
}
GPU_SURFACES = {'tools.scene-runner.gpu', 'graphics.scene.render'}
EXPORT_SURFACES = {'content.client-export.package'}
FEATUREWORLD_CASES = [
    'featureworld.components', 'featureworld.commands', 'featureworld.batches-history',
    'featureworld.retries', 'featureworld.persistence', 'featureworld.asset-identity',
    'featureworld.asset-import', 'featureworld.asset-jobs', 'featureworld.asset-pipeline',
    'featureworld.gltf-source', 'featureworld.gltf-geometry', 'featureworld.gltf-textures',
    'featureworld.gltf-assets', 'featureworld.render-components', 'featureworld.prepared-content',
    'featureworld.scene-extraction', 'featureworld.client-export',
]


def check(condition, message):
    if not condition:
        raise ValueError(message)


def audit(manifest, cpu, gpu, registered, declared):
    errors = validate_contract.validate_manifest(manifest)
    check(not errors, 'Invalid manifest: ' + '; '.join(errors))
    check(not validate_contract.validate_surface_files(manifest, ROOT), 'Missing or escaping fixture/source')
    cases = manifest['featureworld']['case_ids']
    check(cases == FEATUREWORLD_CASES, 'Independent FeatureWorld case inventory drift')
    check(cpu['suites'].get('featureworld') == cases, 'Manifest/native suite drift')
    check([c for c in cpu['cases'] if c.startswith('featureworld.')] == cases, 'Manifest/native case drift')
    gpu_ids = gpu['catalog']['cases']
    check(gpu['format'] == 'shark.scene-execution-cases' and gpu['schema_version'] == 1
          and gpu['mode'] == 'gpu' and gpu['renderer_initialized'] is False
          and len(gpu_ids) == len(set(gpu_ids)), 'Invalid device-free GPU catalog')
    surfaces = manifest['implemented_surface']
    actual = {e['id'] for category in ('components', 'commands') for e in surfaces[category]}
    check(actual == SCENE_SURFACES | GPU_SURFACES | EXPORT_SURFACES, 'Public surface/exercised inventory drift')
    check(not surfaces['lua_api'], 'Lua needs actual binding exercises before coverage is claimed')
    rows = []
    covered = set()
    for category, entries in surfaces.items():
        for entry in entries:
            tests = entry['test_ids']
            check(len(tests) == len(set(tests)) and set(tests) <= registered | declared,
                  'Unknown/duplicate CTest mapping: ' + entry['id'])
            if category in ('components', 'commands'):
                check(set(tests) <= registered, 'Public scene surface lacks tests in this profile: ' + entry['id'])
            check(set(entry.get('gpu_case_ids', [])) <= set(gpu_ids), 'Unknown native GPU case: ' + entry['id'])
            mapped = entry.get('featureworld_cases', [])
            if entry['id'] in SCENE_SURFACES:
                check(mapped, 'Exercised surface lost its FeatureWorld mapping: ' + entry['id'])
                covered.update(mapped)
            if entry['id'] == 'tools.scene-runner.gpu':
                check(not mapped and set(entry.get('gpu_case_ids', [])) == set(gpu_ids)
                      and entry.get('external_fixture_reason', '').strip(), 'GPU external fixture drift')
            if entry['id'] == 'graphics.scene.render':
                check(not mapped and not entry.get('gpu_case_ids')
                      and entry.get('external_fixture_reason', '').strip()
                      and {'tests/scene_render_smoke.cpp', 'tools/scene_render_smoke.py',
                           'tests/runtime_scene_prepare.cpp', 'tests/runtime_scene_lifecycle.cpp',
                           'tools/runtime_scene_smoke.py'} <= set(entry['fixtures'])
                      and {'shark.tooling.scene-render-smoke', 'shark.tooling.runtime-scene-smoke'} <= set(tests),
                      'Scene viewport/Runtime external fixture drift')
            if entry['id'] in EXPORT_SURFACES:
                check(not mapped and entry.get('external_fixture_reason', '').strip()
                      and {'shark.tools.client-export-process', 'shark.tooling.runtime-package'} <= set(tests)
                      and 'tests/runtime_scene_prepare.cpp' in entry['fixtures'],
                      'Standalone export process fixture drift')
            rows.append({'id': entry['id'], 'category': category, 'featureworld_cases': mapped,
                         'registered_tests': sorted(set(tests) & registered),
                         'declared_other_profile_tests': sorted(set(tests) - registered)})
    check(covered == set(cases), 'Native FeatureWorld case has no public surface exercise')
    return rows


def inspect_execution(value, output, status):
    check(value['format'] == 'shark.scene-execution' and type(value['schema_version']) is int
          and value['schema_version'] == 1 and value['mode'] == 'cpu'
          and value['renderer_initialized'] is False and value['status'] == status,
          'Invalid supervisor result')
    check(value == scene_runner.read_json(output/'result.json'), 'Published result differs from stdout')
    check(value['artifacts'] == scene_runner.artifacts(output), 'Artifact inventory/hash drift')
    check(value['cpu'] == scene_runner.read_json(output/'native-result.json'), 'Native publication drift')
    check(value['cpu']['status'] == status and value['cpu']['renderer_initialized'] is False,
          'Native/supervisor status drift')
    check(value['run_id'] == 'qualification' and value['revision'] == 'qualification', 'Caller context lost')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    summary = {'format': 'shark.scene-qualification', 'schema_version': 1, 'status': 'FAIL',
               'gpu_qualification': 'NOT_RUN', 'commands': [], 'negative_controls': []}
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    def call(name, command, expected=0):
        process = run_process([str(a) for a in command], cwd=ROOT, timeout=15)
        summary['commands'].append({'name': name, 'exit_code': process.returncode,
                                    'timed_out': process.timed_out, 'cleanup_errors': list(process.cleanup_errors)})
        check(process.returncode == expected and not process.timed_out and not process.cleanup_errors
              and not process.stderr.strip(), name + ': ' + process.stdout[-2048:] + process.stderr[-1024:])
        return scene_runner.decode(process.stdout)

    def reject(name, action):
        try:
            action()
        except (ValueError, scene_runner.RequestError):
            summary['negative_controls'].append(name)
        else:
            raise ValueError('False coverage/result accepted: ' + name)

    try:
        manifest = scene_runner.read_json(ROOT/'docs/coverage/requirements.json')
        cpu = scene_runner.cpu_catalog(args.runner.resolve())
        gpu = call('gpu-device-free-discovery', [sys.executable, ROOT/'tools/scene_runner.py', '--mode', 'gpu', '--list-cases'])
        # CTest discovery writes Testing/Temporary even with --show-only. Read
        # the real generated registration tree through a private metadata root,
        # so nested discovery cannot overwrite its enclosing CTest's logs.
        with tempfile.TemporaryDirectory(prefix='shark-ctest-discovery-', dir=output.parent) as temporary:
            discovery = Path(temporary)
            build = args.build_dir.resolve().as_posix()
            check(']=]' not in build, 'Unsupported CMake bracket delimiter in build path')
            (discovery/'CTestTestfile.cmake').write_text('subdirs([=['+build+']=])\n', encoding='utf-8')
            inventory = call('current-ctest-inventory', ['ctest', '--test-dir', discovery, '--show-only=json-v1'])
        registered = {t['name'] for t in inventory['tests']}
        # Literal declarations identify optional/other-profile fixtures, not
        # passing executions. The closing gate separately checks actual results.
        declared = set()
        for path in (ROOT/'tests/CMakeLists.txt', ROOT/'probes/graphics/CMakeLists.txt'):
            source = path.read_text(encoding='utf-8')
            for name in re.findall(r'add_test\s*\(\s*NAME\s+"?([^"\s)]+)', source):
                if '${shark_application_lower}' in name:
                    applications = re.search(r'foreach\(shark_application IN ITEMS ([A-Za-z ]+)\)', source)
                    check(applications is not None, 'Unknown application registration template')
                    declared.update(name.replace('${shark_application_lower}', app.lower())
                                    for app in applications[1].split())
                elif re.fullmatch(r'shark\.[a-z0-9.-]+', name):
                    declared.add(name)
        summary['coverage'] = audit(manifest, cpu, gpu, registered, declared)
        summary['scene_surfaces'] = sorted(SCENE_SURFACES)
        summary['external_gpu_surfaces'] = sorted(GPU_SURFACES)
        summary['external_export_surfaces'] = sorted(EXPORT_SURFACES)
        summary['source_sha256'] = {name: scene_runner.sha(ROOT/name) for name in (
            'docs/coverage/requirements.json', 'assets/FeatureWorld/FeatureWorld.sharkproject',
            'assets/FeatureWorld/scenes/FeatureWorld.sharkscene')}
        mutations = {
            'missing-surface': lambda m: m['implemented_surface']['components'].pop(),
            'stale-test': lambda m: m['implemented_surface']['commands'][1]['test_ids'].append('shark.unknown.fixture'),
            'duplicate-test': lambda m: m['implemented_surface']['commands'][1]['test_ids'].append(m['implemented_surface']['commands'][1]['test_ids'][0]),
            'missing-test': lambda m: m['implemented_surface']['commands'][1].update(test_ids=[]),
            'stale-gpu-case': lambda m: m['implemented_surface']['commands'][0].update(gpu_case_ids=['gpu.unknown']),
            'missing-gpu-reason': lambda m: m['implemented_surface']['commands'][0].pop('external_fixture_reason'),
            'missing-scene-mapping': lambda m: m['implemented_surface']['components'][0].update(featureworld_cases=[]),
            'missing-fixture': lambda m: m['implemented_surface']['components'][0]['fixtures'].append('assets/FeatureWorld/missing.fixture'),
        }
        for name, mutate in mutations.items():
            changed = copy.deepcopy(manifest); mutate(changed)
            reject(name, lambda: audit(changed, cpu, gpu, registered, declared))
        stale = copy.deepcopy(cpu); stale['suites']['featureworld'] = stale['suites']['featureworld'][:-1]
        reject('native-suite-drift', lambda: audit(manifest, stale, gpu, registered, declared))
        stale_gpu = copy.deepcopy(gpu); stale_gpu['catalog']['cases'].append('gpu.unknown')
        reject('gpu-discovery-drift', lambda: audit(manifest, cpu, stale_gpu, registered, declared))
        reject('missing-profile-tests', lambda: audit(manifest, cpu, gpu, set(), declared))
        with tempfile.TemporaryDirectory(prefix='shark-scene-qualification-', dir=output.parent) as temporary:
            folder = Path(temporary); scratch = folder/'scratch'; scratch.mkdir()
            project = folder/'caf\u00e9 \u4e16\u754c'; (project/'scenes').mkdir(parents=True)
            fixture = ROOT/'assets/FeatureWorld'
            source_paths = (Path('FeatureWorld.sharkproject'), Path('scenes/FeatureWorld.sharkscene'))
            source_hashes = {p: scene_runner.sha(fixture/p) for p in source_paths}
            for path in source_paths:
                shutil.copyfile(fixture/path, project/path)
            base = [sys.executable, ROOT/'tools/scene_runner.py', '--mode', 'cpu', '--executable', args.runner.resolve(),
                    '--run-id', 'qualification', '--revision', 'qualification', '--seed', '1337', '--ticks', '3', '--hz', '20']
            good = call('saved-world-through-supervisor', [*base, '--case', 'featureworld', '--project',
                        project/'FeatureWorld.sharkproject', '--workspace', scratch, '--output', folder/'good'])
            inspect_execution(good, folder/'good', 'PASS')
            check([c['id'] for c in good['cpu']['cases']] == manifest['featureworld']['case_ids']
                  and all(c['status'] == 'PASS' and c['ticks_completed'] == 3 and c['assertions'] > 0
                          for c in good['cpu']['cases']), 'Mapped cases did not all execute successfully')
            failed = call('assertion-through-supervisor', [*base, '--case', 'intentional-failure', '--output', folder/'failed'], 1)
            inspect_execution(failed, folder/'failed', 'FAIL')
            case = failed['cpu']['cases'][0]
            check(len(failed['cpu']['cases']) == 1 and case['id'] == 'intentional-failure' and case['status'] == 'FAIL'
                  and failed['exit_code'] == 1 and failed['process']['exit_code'] == 1
                  and case['failure']['kind'] == 'assertion' and case['failure']['phase'] == 'tick'
                  and case['failure']['tick'] == 0
                  and case['failure']['error']['entity_id'] == '10000000-0000-4000-8000-000000000004',
                  'Assertion lost failure, tick, phase, case or entity context')
            for field, value in [('status', 'PASS'), ('renderer_initialized', True), ('artifacts', [])]:
                altered = copy.deepcopy(failed); altered[field] = value
                reject('altered-' + field, lambda: inspect_execution(altered, folder/'failed', 'FAIL'))
            native = folder/'failed/native-result.json'; original = native.read_bytes()
            native.write_bytes(original+b' ')
            reject('altered-native-bytes', lambda: inspect_execution(failed, folder/'failed', 'FAIL'))
            native.write_bytes(original)
            inspect_execution(failed, folder/'failed', 'FAIL')
            summary['executions'] = {'passing': good, 'intentional_failure': failed}
            summary['evidence_scope'] = 'Published files, stdout, native payloads and all artifact hashes were compared before managed temporary cleanup; snapshots are embedded here.'
            check(not list(scratch.iterdir()), 'Saved-world scratch leaked')
            check(all(scene_runner.sha(fixture/p) == source_hashes[p] and scene_runner.sha(project/p) == source_hashes[p]
                      for p in source_paths), 'Saved-world input changed')
        summary['status'] = 'PASS'
    except Exception as error:
        summary['error'] = repr(error)
    summary['checks'] = len(summary['commands']) + len(summary['negative_controls'])
    output.write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({**{k: summary[k] for k in ('status', 'checks', 'negative_controls')}, 'error': summary.get('error')}))
    return 0 if summary['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
