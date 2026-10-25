"""CPU-only supervisor/evidence controls. Synthetic readbacks qualify no GPU."""
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
import argparse
import io
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import scene_runner as runner
import offscreen_smoke
import cube_smoke
import test_offscreen_smoke as offscreen_fixture
import test_cube_smoke as window_fixture
from process_runner import ProcessResult


class SupervisorTests(unittest.TestCase):
    def test_gpu_discovery_is_device_free_and_never_imports_native_helpers(self):
        with patch.object(runner, 'gpu_preflight', side_effect=AssertionError('GPU touched')):
            with io.StringIO() as output, patch('sys.stdout', output):
                self.assertEqual(runner.main(['--mode','gpu','--list-cases']), 0)
                value = json.loads(output.getvalue())
        self.assertFalse(value['renderer_initialized'])
        self.assertEqual(value['catalog']['cases'], list(runner.GPU_CASES))

    def test_invalid_or_cross_mode_arguments_are_rejected(self):
        base = ['--mode','gpu','--case','gpu.offscreen-cube','--device','a'*32,'--output','unused']
        variants = [[], ['--mode','future'], ['--mode','cpu','--output','x','--mode','cpu'],
            ['--mode','cpu','--output','x','--device','a'*32],
            ['--mode','cpu','--output','x','--ticks','0'], ['--mode','cpu','--output','x','--seed','-1'],
            ['--mode','cpu','--output','x','--seed',str(2**64)], ['--mode','cpu','--output','x','--hz','1001'],
            [*base,'--seed','1337'], [*base,'--ticks','3'], [*base,'--input','0:offset:1'],
            [*base,'--project','x'], [*base,'--bundle','x'], [*base,'--timeout','nan'],
            [*base,'--timeout','181'], [*base,'--run-id','bad value'],
            ['--mode','gpu','--case','gpu.offscreen-cube','--device','auto','--output','x'],
            ['--mode','gpu','--list-cases','--output','x'], ['--mode','gpu','--list-cases','--executable','x']]
        for args in variants:
            with self.subTest(args=args), self.assertRaises(runner.RequestError):
                runner.parse(args)

    def test_unknown_json_duplicate_keys_nonfinite_and_oversize_are_rejected(self):
        for text in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":1e999}', 'x'*(runner.MAX_JSON+1)):
            with self.subTest(text=text[:30]), self.assertRaises((ValueError,runner.RequestError)):
                runner.decode(text)

    def test_cpu_catalog_cannot_fake_or_omit_native_cases(self):
        good = {'format':'shark.scene-cases','schema_version':1,'mode':'cpu','cases':['foundation'],'suites':{}}
        process = ProcessResult([],0,json.dumps(good),'',False)
        with patch.object(runner,'run_process',return_value=process):
            self.assertEqual(runner.cpu_catalog(Path('worker')),good)
        for field,value in (('schema_version',True),('schema_version',2),('mode','gpu'),('cases',[]),
                            ('cases',['a','a']),('suites',{'suite':['missing']})):
            bad = {**good,field:value}
            with self.subTest(field=field,value=value), patch.object(runner,'run_process',
                    return_value=replace(process,stdout=json.dumps(bad))), self.assertRaises(runner.RequestError):
                runner.cpu_catalog(Path('worker'))

    def test_cpu_report_rejects_false_renderer_mode_context_progress_and_status(self):
        args = runner.parse(['--mode','cpu','--output','unused','--ticks','1'])
        case = {'id':'foundation','status':'PASS','assertions':1,'ticks_completed':1,'random_draws':0,
                'failure':None,'initial_revision':{'instance':1,'value':0},'final_revision':{'instance':1,'value':1}}
        value = {'format':'shark.scene-run','schema_version':1,'mode':'cpu','renderer_initialized':False,
                 'status':'PASS','run_id':'local','revision':'unspecified','seed':1337,'ticks':1,
                 'tick_rate_hz':60,'cases':[case],'inputs':[]}
        process = ProcessResult([],0,json.dumps(value),'',False)
        self.assertEqual(runner.cpu_report(process,args,['foundation']),value)
        changes = [lambda v:v.update(renderer_initialized=True),lambda v:v.update(mode='gpu'),
            lambda v:v.update(seed=True),lambda v:v.update(revision='other'),lambda v:v.update(cases=[]),
            lambda v:v.update(status='FAIL'),lambda v:v['cases'][0].update(assertions=0),
            lambda v:v['cases'][0].update(ticks_completed=0),lambda v:v['cases'][0].update(random_draws=65537),
            lambda v:v['cases'][0]['initial_revision'].update(instance=0),
            lambda v:v.update(inputs=[{'tick':0,'action':'offset','value':1}])]
        for change in changes:
            bad = deepcopy(value); change(bad)
            with self.subTest(change=change), self.assertRaises(runner.RequestError):
                runner.cpu_report(replace(process,stdout=json.dumps(bad)),args,['foundation'])

    def test_create_only_complete_publication_preserves_existing_bytes(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name); path=root/'result.json'
            runner.publish(path,{'value':1})
            before=path.read_bytes()
            with self.assertRaises(FileExistsError): runner.publish(path,{'value':2})
            self.assertEqual(path.read_bytes(),before)
            self.assertEqual(list(root.iterdir()),[path])

    def test_artifact_limits_links_and_junctions_are_explicit(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name); (root/'keep').write_bytes(b'keep')
            with patch.object(runner,'MAX_ARTIFACTS',0), self.assertRaises(runner.RequestError):
                runner.artifacts(root)
            with patch.object(runner,'MAX_ARTIFACT_BYTES',3), self.assertRaises(runner.RequestError):
                runner.artifacts(root)
            with patch.object(Path,'is_symlink',return_value=True), self.assertRaises(runner.RequestError):
                runner.no_links(root/'child')
            self.assertEqual((root/'keep').read_bytes(),b'keep')

    def test_unavailable_platform_never_launches_or_falls_back(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name)
            args=['--mode','gpu','--case','gpu.offscreen-cube','--device','a'*32,'--output',str(root/'run')]
            with patch.object(runner.sys,'platform','linux'), patch.object(runner,'run_process',side_effect=AssertionError('launched')):
                with io.StringIO() as output, patch('sys.stdout',output):
                    self.assertEqual(runner.main(args),3)
                    result=json.loads(output.getvalue())
            self.assertEqual(result['status'],'NOT_RUN')
            self.assertFalse(result['renderer_initialized'])
            self.assertEqual(json.loads((root/'run/result.json').read_text()),result)

    def test_deadline_and_cancellation_preserve_failure_without_cpu_fallback(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name); exe=root/'worker';exe.write_bytes(b'trusted fixture')
            args=['--mode','cpu','--executable',str(exe),'--output',str(root/'run')]
            catalog={'cases':['foundation'],'suites':{}}
            for index,effect in enumerate((ProcessResult([],124,'partial','',True),runner.Cancelled())):
                changed=list(args);changed[-1]=str(root/str(index))
                with patch.object(runner,'cpu_catalog',return_value=catalog), patch.object(runner,'run_process') as run:
                    if isinstance(effect,BaseException):run.side_effect=effect
                    else:run.return_value=effect
                    with io.StringIO() as output,patch('sys.stdout',output):
                        code=runner.main(changed);result=json.loads(output.getvalue())
                self.assertEqual(code,124 if index==0 else 130)
                self.assertEqual(result['status'],'FAIL' if index==0 else 'CANCELLED')
                self.assertFalse(result['renderer_initialized'])


class GPUReceiptTests(unittest.TestCase):
    """Use independent CPU oracles to mutate receipts; never create a native device."""
    def setUp(self):
        temporary=tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name); self.directory=self.root/'native';self.directory.mkdir()
        self.readbacks=self.directory/'readbacks';self.readbacks.mkdir()
        self.exe=self.root/'fixture.exe';self.exe.write_bytes(b'CPU fixture, no GPU')
        self.bundle=self.root/'cube.sharkshader';self.bundle.write_bytes(b'CPU fixture bundle')

    def prepare(self,window=False):
        module=cube_smoke if window else offscreen_smoke
        rows=window_fixture.records() if window else offscreen_fixture.records()
        inputs=module.source_inputs();digest=module.foundation.source_digest(inputs)
        rows[0]['source_sha256']=digest
        rows[0]['dependency_lock_sha256']=offscreen_fixture.LOCK
        if window:rows[1]['bundle_sha256']=runner.sha(self.bundle)
        if window:
            for index,extent in enumerate(cube_smoke.EXTENTS):
                for target in ('window','offscreen'):
                    ref=cube_smoke.srgb_reference(extent) if target=='window' else offscreen_smoke.rasterize(0,extent)
                    (self.readbacks/f'{target}-{index}.rgba8').write_bytes(ref.color)
                    (self.readbacks/f'{target}-{index}.d32f').write_bytes(struct.pack(f'<{len(ref.depth)}f',*ref.depth))
        else:
            for index in range(2):
                data=offscreen_smoke.fixture(index);ref=offscreen_smoke.rasterize(index)
                (self.readbacks/data['color_file']).write_bytes(ref.color)
                (self.readbacks/data['depth_file']).write_bytes(struct.pack(f'<{len(ref.depth)}f',*ref.depth))
        text='\n'.join(json.dumps(row) for row in rows)
        (self.directory/'stdout.jsonl').write_text(text)
        (self.directory/'stderr.log').write_text('')
        native=ProcessResult([],0,text,'',False)
        args=argparse.Namespace(case='gpu.window-cube' if window else 'gpu.offscreen-cube',
                               device=window_fixture.UUID if window else offscreen_fixture.UUID,
                               executable=self.exe,bundle=self.bundle)
        preflight={'dependency_lock_sha256':offscreen_fixture.LOCK}
        if window:
            _,problems,comparisons=module.inspect_result(native,'smoke',args.device,'required',
                preflight['dependency_lock_sha256'],digest,runner.sha(self.bundle),self.readbacks)
        else:
            _,problems,comparisons=module.inspect_result(native,'render',args.device,'required',
                preflight['dependency_lock_sha256'],digest,self.readbacks)
        self.assertEqual(problems,[])
        summary={'schema_version':1,'exit_code':0,'status':'PASS','qualification':'PASS','validation':'required',
                 'dependency_lock_sha256':preflight['dependency_lock_sha256'],'executable_sha256':runner.sha(self.exe),
                 'input_sha256':inputs,'source_sha256':digest,'process_exit_code':0,'timed_out':False,
                 'cleanup_errors':[],'command':[],'records':rows,'comparisons':comparisons}
        return summary,native,args,preflight

    def test_offscreen_receipt_binds_source_device_and_raw_comparisons(self):
        summary,process,args,preflight=self.prepare()
        value=runner.gpu_report(summary,process,args,self.directory,preflight)
        self.assertEqual(len(value['comparisons']),2)
        changes=[lambda v:v.update(schema_version=True),lambda v:v.update(status='NOT_RUN'),
            lambda v:v.update(qualification='NOT_RUN'),lambda v:v.update(validation='off'),
            lambda v:v.update(source_sha256='f'*64),lambda v:v.update(executable_sha256='f'*64),
            lambda v:v.update(timed_out=True),lambda v:v.update(process_exit_code=False),
            lambda v:v.update(records=[]),lambda v:v.update(comparisons=[])]
        for change in changes:
            bad=deepcopy(summary);change(bad)
            with self.subTest(change=change),self.assertRaises(runner.RequestError):
                runner.gpu_report(bad,process,args,self.directory,preflight)
        args.device='f'*32
        with self.assertRaises(runner.RequestError):runner.gpu_report(summary,process,args,self.directory,preflight)

    def test_window_receipt_requires_paired_capture_resize_minimize_and_retirement(self):
        summary,process,args,preflight=self.prepare(window=True)
        value=runner.gpu_report(summary,process,args,self.directory,preflight)
        self.assertEqual(len(value['comparisons']),6)
        rows=deepcopy(summary['records']);rows[-1]['resources']['window_draw_resources']=1
        (self.directory/'stdout.jsonl').write_text('\n'.join(json.dumps(row) for row in rows))
        with self.assertRaises(runner.RequestError):runner.gpu_report(summary,process,args,self.directory,preflight)

    def test_forged_pass_missing_and_corrupt_readbacks_never_qualify(self):
        summary,process,args,preflight=self.prepare()
        path=self.readbacks/'cube-96x80.rgba8'
        path.write_bytes(bytes(path.stat().st_size))
        with self.assertRaises(runner.RequestError):runner.gpu_report(summary,process,args,self.directory,preflight)
        path.unlink()
        with self.assertRaises(runner.RequestError):runner.gpu_report(summary,process,args,self.directory,preflight)

    def test_unavailable_receipt_and_nonzero_exit_cannot_be_promoted_to_pass(self):
        summary,process,args,preflight=self.prepare()
        with self.assertRaises(runner.CapabilityUnavailable):
            runner.gpu_report({**summary,'status':'NOT_RUN'},replace(process,returncode=3),args,self.directory,preflight)
        for process in (replace(process,returncode=1),replace(process,returncode=124,timed_out=True)):
            with self.assertRaises(runner.RequestError):runner.gpu_report(summary,process,args,self.directory,preflight)

    def test_native_capability_failure_retains_explicit_unavailable_classification(self):
        summary,process,args,preflight=self.prepare()
        rows=[summary['records'][0],{'kind':'failure','error':{'code':'capability_unavailable',
                                                           'message':'Selected UUID is unavailable'}}]
        (self.directory/'stdout.jsonl').write_text('\n'.join(json.dumps(row) for row in rows))
        with self.assertRaises(runner.CapabilityUnavailable):
            runner.gpu_report(summary,replace(process,returncode=1),args,self.directory,preflight)


if __name__=='__main__':
    unittest.main()
