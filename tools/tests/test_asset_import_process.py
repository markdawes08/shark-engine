"""Interrupt actual cache publication, reopen and recover the same project."""
from pathlib import Path
import argparse
import hashlib
import json
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--probe',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();output=args.output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
    rows=[];result={'status':'FAIL','records':rows}
    def call(name,mode,root,*extra,expected=0):
        process=run_process([str(args.probe.resolve()),mode,str(root),*extra],timeout=15)
        row={'name':name,'exit_code':process.returncode,'timed_out':process.timed_out,'cleanup_errors':list(process.cleanup_errors),'stdout':process.stdout,'stderr':process.stderr}
        rows.append(row)
        assert process.returncode==expected and not process.timed_out and not process.cleanup_errors,(name,row)
        if expected==77:
            assert not process.stdout.strip() and not process.stderr.strip()
            return
        assert not process.stderr.strip(),row
        value=json.loads(process.stdout);assert value['status']=='PASS' and value['format']=='shark.asset-import-process' and value['schema_version']==1
        return value
    def check(name,condition):
        rows.append({'name':name,'status':'PASS' if condition else 'FAIL'});assert condition,name
    try:
        with tempfile.TemporaryDirectory(prefix='shark-import-process-',dir=output.parent) as temporary:
            parent=Path(temporary)
            for phase in ('staged','written','flushed','before_publish','published'):
                root=parent/(phase+'-caf\u00e9 \u4e16\u754c');root.mkdir()
                first=call(phase+'-seed','seed',root)
                cache=root/'.shark-development-cache';before=cache.read_bytes()
                metadata=root/'asset.bin.sharkasset';source_metadata=metadata.read_bytes()
                source_stage=root/'.shark-stage-10000000-0000-4000-8000-000000000001';source_stage.write_bytes(b'authored recovery evidence')
                call(phase+'-interrupt','interrupt',root,phase,expected=77)
                committed=phase=='published'
                observed=call(phase+'-reopen','inspect-after' if committed else 'inspect-before',root)
                check(phase+'-atomic-cache', (cache.read_bytes()!=before)==committed and observed['payload']==('After' if committed else 'Before'))
                check(phase+'-source-preserved',metadata.read_bytes()==source_metadata and (root/'asset.bin').read_bytes()==b'After')
                stages=list(root.glob('.shark-cache-stage-*'))
                check(phase+'-stage-count',len(stages)==(0 if committed else 1))
                if not committed:call(phase+'-orphan-admission','blocked',root)
                recovered=call(phase+'-recover','recover',root)
                check(phase+'-recovery-result',recovered['recovered']==(0 if committed else 1) and recovered['payload']=='After' and recovered['fresh'] and recovered['key']!=first['key'])
                check(phase+'-recovery-cleanup',not list(root.glob('.shark-cache-stage-*')) and source_stage.read_bytes()==b'authored recovery evidence')
                repeated=call(phase+'-repeat-recovery','recover',root)
                check(phase+'-idempotent-recovery',repeated['recovered']==0 and repeated['key']==recovered['key'])
                result.setdefault('final_snapshots',[]).append({'phase':phase,'sha256':hashlib.sha256(cache.read_bytes()).hexdigest(),'bytes':cache.stat().st_size,'key':recovered['key']})
        result['status']='PASS'
    except Exception as error:result['error']=repr(error)
    result['checks']=len(rows);output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:result.get(k) for k in ('status','checks','error')}));return 0 if result['status']=='PASS' else 1
if __name__=='__main__':raise SystemExit(main())
