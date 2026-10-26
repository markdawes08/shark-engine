"""Real supervisor/CPU-worker checks; GPU capability failures never access a device."""
from pathlib import Path
import argparse
import copy
import hashlib
import json
import shutil
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from process_runner import run_process
import scene_runner


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    output=args.output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
    records=[];summary={'status':'FAIL','records':records,'gpu_qualification':'NOT_RUN'}

    def call(name,options,expected,catalog=False):
        process=run_process([sys.executable,str(scene_runner.ROOT/'tools/scene_runner.py'),*options],
                            cwd=output.parent,timeout=15)
        assert not process.timed_out and not process.cleanup_errors and not process.stderr.strip(),name
        assert process.returncode==expected,(name,process.stdout,process.stderr)
        result=scene_runner.decode(process.stdout)
        assert result['format']==('shark.scene-execution-cases' if catalog else 'shark.scene-execution')
        assert type(result['schema_version']) is int and result['schema_version']==1
        if not catalog:
            assert result['exit_code']==expected
            if expected==0:assert result['status']=='PASS'
            if result.get('artifact_root'):
                root=Path(result['artifact_root'])
                assert scene_runner.read_json(root/'result.json')==result
                for row in result['artifacts']:
                    path=root/row['path']
                    assert path.is_file() and path.stat().st_size==row['bytes'] and scene_runner.sha(path)==row['sha256']
        records.append({'name':name,'exit_code':process.returncode})
        return result

    try:
        with tempfile.TemporaryDirectory(prefix='shark-scene-execution-',dir=output.parent) as name:
            folder=Path(name); worker=args.runner.resolve()
            base=['--mode','cpu','--executable',str(worker),'--ticks','3','--hz','59','--seed','1337',
                  '--input','0:offset:5','--input','2:offset:-2','--revision','process-test','--run-id','supervised']
            first=call('cpu-foundation',[*base,'--output',str(folder/'first')],0)
            second=call('cpu-foundation-repeat',[*base,'--output',str(folder/'repeat')],0)
            def semantic(value):
                result=copy.deepcopy(value['cpu']);result.pop('artifacts')
                for case in result['cases']:
                    for field in ('initial_revision','final_revision'):case[field].pop('instance')
                return result
            assert semantic(first)==semantic(second) and first['renderer_initialized'] is False
            assert first['cpu']['cases'][0]['ticks_completed']==3
            failure=call('cpu-intentional-failure',['--mode','cpu','--executable',str(worker),'--case','intentional-failure',
                                                   '--output',str(folder/'failure')],1)
            assert failure['status']=='FAIL' and failure['cpu']['cases'][0]['failure']['tick']==0
            assert failure['cpu']['cases'][0]['failure']['error']['entity_id']=='10000000-0000-4000-8000-000000000004'
            cpu=call('cpu-catalog',['--mode','cpu','--list-cases','--executable',str(worker)],0,True)
            gpu=call('gpu-catalog',['--mode','gpu','--list-cases'],0,True)
            assert 'featureworld' in cpu['catalog']['suites'] and gpu['catalog']['cases']==list(scene_runner.GPU_CASES)
            assert gpu['renderer_initialized'] is False
            manifest=json.loads((scene_runner.ROOT/'docs/coverage/requirements.json').read_text(encoding='utf-8'))
            surface=next(e for e in manifest['implemented_surface']['commands'] if e['id']=='tools.scene-runner.gpu')
            assert surface['gpu_case_ids']==gpu['catalog']['cases'] and surface['external_fixture_reason'].strip()
            fixture=scene_runner.ROOT/'assets/FeatureWorld'
            project=folder/'caf\u00e9 \u4e16\u754c';(project/'scenes').mkdir(parents=True)
            paths=[Path('FeatureWorld.sharkproject'),Path('scenes/FeatureWorld.sharkscene')]
            original={p.as_posix():(fixture/p).read_bytes() for p in paths}
            for path in paths:shutil.copyfile(fixture/path,project/path)
            scratch=folder/'scratch';scratch.mkdir()
            saved=call('saved-featureworld',['--mode','cpu','--executable',str(worker),'--case','featureworld',
                 '--project',str(project/'FeatureWorld.sharkproject'),'--workspace',str(scratch),'--ticks','3',
                 '--output',str(folder/'saved')],0)
            assert len(saved['cpu']['cases'])==6 and not list(scratch.iterdir())
            for path in paths:assert (project/path).read_bytes()==original[path.as_posix()]
            sentinel=folder/'first/result.json';before=sentinel.read_bytes()
            conflict=call('no-overwrite',[*base,'--output',str(folder/'first')],2)
            assert conflict['error']['code']=='conflict' and sentinel.read_bytes()==before
            call('missing-output-parent',[*base,'--output',str(folder/'absent'/'new')],2)
            missing=call('missing-cpu-worker',['--mode','cpu','--executable',str(folder/'missing'),
                                              '--output',str(folder/'missing-worker')],3)
            assert missing['status']=='NOT_RUN' and missing['renderer_initialized'] is False
            native_error=call('native-input-error',['--mode','cpu','--executable',str(worker),'--input','bad',
                                                   '--output',str(folder/'bad-input')],2)
            assert native_error['status']=='ERROR' and native_error['cpu']['status']=='ERROR'
            unavailable=call('gpu-prerequisite-unavailable',['--mode','gpu','--case','gpu.offscreen-cube',
                      '--device','f'*32,'--executable',str(folder/'missing.exe'),'--output',str(folder/'gpu-missing')],3)
            assert unavailable['status']=='NOT_RUN' and unavailable['renderer_initialized'] is False
            invalids=[['--mode','future'],['--mode','gpu','--case','gpu.window-cube','--device','auto','--output',str(folder/'unused')],
                ['--mode','cpu','--device','f'*32,'--output',str(folder/'unused')],
                ['--mode','gpu','--case','gpu.offscreen-cube','--device','f'*32,'--ticks','3','--output',str(folder/'unused')],
                ['--mode','cpu','--timeout','inf','--output',str(folder/'unused')],
                ['--mode','cpu','--seed',str(2**64),'--output',str(folder/'unused')],
                ['--mode','cpu','--hz','0','--output',str(folder/'unused')],
                ['--mode','cpu','--run-id','bad value','--output',str(folder/'unused')],
                ['--mode','cpu','--list-cases','--output',str(folder/'unused')]]
            for index,options in enumerate(invalids):call('invalid-'+str(index),options,2)
            assert not (folder/'unused').exists()
            try:
                linked=folder/'linked';linked.symlink_to(folder/'first',target_is_directory=True)
            except OSError:
                summary['symlink_control']='NOT_RUN'
            else:
                call('linked-output-parent',[*base,'--output',str(linked/'new')],2)
                assert sentinel.read_bytes()==before
            assert not list(folder.rglob('.shark-run-stage-*'))
        summary['status']='PASS'
    except Exception as error:summary['error']=repr(error)
    summary['checks']=len(records)
    output.write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(summary))
    return 0 if summary['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
