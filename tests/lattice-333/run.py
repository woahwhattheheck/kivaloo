#!/usr/bin/env python3
"""Exact-source cleanup validation for Tarsnap/kivaloo PR 333.
Uses temporary public fixtures; never connects to cloud services.
"""
from __future__ import annotations
import hashlib,json,os,shutil,subprocess,sys,tarfile,tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parent
DISPATCH_C=(HERE/'dispatch_probe.c').read_text()
DIRECTORY_C=(HERE/'directory_probe.c').read_text()

BASE = '3de151b5d878b0714552c3658562eb5a87b378ce'
HEAD = '9b517f6df327737183328b9d307b21ee4b613988'
SOURCES = ('lbs/dispatch.c', 'lbs-s3/dispatch.c', 'lbs-dynamodb/dispatch.c', 'lbs/storage_findfiles.c')
MODES = ('success', 'eio-released', 'eintr-released-reused', 'eintr-retained',
         'einprogress-released', 'ebadf', 'success-stale-eintr')
REPO = Path(subprocess.check_output(['git','rev-parse','--show-toplevel'], text=True).strip())
OUT = Path(sys.argv[1] if len(sys.argv)>1 else 'lattice-333-results').resolve()
OUT.mkdir(parents=True, exist_ok=False)
RESULTS = []


def command(args, label, cwd, timeout=240, env=None):
    cp = subprocess.run(args, cwd=cwd, capture_output=True, timeout=timeout, env=env)
    (OUT / (label+'.stdout')).write_bytes(cp.stdout)
    (OUT / (label+'.stderr')).write_bytes(cp.stderr)
    print('COMMAND', label, cp.returncode, flush=True)
    return cp


def must(args, label, cwd):
    cp=command(args, label, cwd)
    if cp.returncode:
        print(cp.stdout.decode(errors='replace')[-3000:])
        print(cp.stderr.decode(errors='replace')[-8000:])
        raise RuntimeError(label+' failed')
    return cp


def record(label, expected, actual):
    row={'case':label, 'expected':expected, 'observed':actual,
         'pass': all(actual.get(k)==v for k,v in expected.items())}
    RESULTS.append(row)
    (OUT/'results.json').write_text(json.dumps(RESULTS, indent=2)+'\n')
    print('RESULT '+json.dumps(row,sort_keys=True), flush=True)


def probe(binary, args, label, expected):
    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
             UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    cp=command([str(binary),*map(str,args)],label,binary.parent,timeout=10,env=env)
    try:
        result=json.loads(cp.stdout)
    except (ValueError,UnicodeError):
        result={'invalid_probe_output':cp.stdout.decode(errors='replace')[-2000:]}
    result['process_exit']=cp.returncode
    result['sanitizer_error']=(b'AddressSanitizer' in cp.stderr or b'runtime error:' in cp.stderr)
    expected={**expected,'sanitizer_error':False}
    record(label,expected,result)


def dispatch_expected(rev, site, mode, worker_error=0):
    done=site in ('lbs-done-write','lbs-done-read')
    hard=mode in (1,4,5)
    guard=rev=='base' and done and hard
    if guard:
        return {'process_exit':90,'guard':1,'target_calls':9,'state_frees':0,
                'worker_calls':3,'cancel_calls':1,'array_frees':1,'fd_open':0}
    twice=rev=='base' and mode in (2,3)
    state_free = done or (site in ('s3-done','dynamodb-done') and not hard)
    return {'process_exit':0,'guard':0,'status':-1 if hard or worker_error else 0,
            'target_calls':2 if twice else 1,
            'all_calls':(2 if done else 1)+(1 if twice else 0),
            'fd_open':int(rev=='head' and mode in (2,3)),
            'warnings':3 if worker_error else int(hard),
            'worker_calls':3 if done else 0,'cancel_calls':int(done),
            'read_frees':int(not done),'write_frees':int(not done),
            'state_frees':int(state_free),'array_frees':2 if done else 0}


def main():
    for name in ('git','cc','make'):
        if not shutil.which(name): raise RuntimeError('Missing '+name)
        must([name,'--version'],'version-'+name,REPO)
    manifest={'base':BASE,'head':HEAD,'original_sources':{},
              'fault_injection':True,'real_cloud_services':False,
              'leak_sanitizer_disabled':'Intentional base/cleanup-error retained state; this is not leak certification.'}
    for revision,sha in (('base',BASE),('head',HEAD)):
        manifest['original_sources'][revision]={path:subprocess.check_output(
            ['git','rev-parse',f'{sha}:{path}'],cwd=REPO,text=True).strip() for path in SOURCES}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (OUT/'os-release.txt').write_bytes(Path('/etc/os-release').read_bytes())
    (OUT/'dispatch_probe.c').write_text(DISPATCH_C)
    (OUT/'directory_probe.c').write_text(DIRECTORY_C)
    with tempfile.TemporaryDirectory(prefix='lattice-kivaloo333-') as td:
        root=Path(td)
        for revision,sha in (('base',BASE),('head',HEAD)):
            build=root/revision; build.mkdir()
            archive=root/(revision+'.tar')
            must(['git','archive','--format=tar','--output='+str(archive),sha],revision+'-archive',REPO)
            with tarfile.open(archive) as tf: tf.extractall(build,filter='data')
            before={p:hashlib.sha256((build/p).read_bytes()).hexdigest() for p in SOURCES}
            must(['make','-j2','PROGS=lbs lbs-s3 lbs-dynamodb','TESTS='],revision+'-native-build',build)
            record(revision+'-three-component-build', {'source_unchanged':True},
                   {'source_unchanged':before=={p:hashlib.sha256((build/p).read_bytes()).hexdigest() for p in SOURCES},
                    'source_sha256':before})
            incdirs=sorted({p.parent for p in build.rglob('*.h')})
            flags=['cc','-std=c99','-O1','-g3','-D_GNU_SOURCE=1','-ffunction-sections','-fdata-sections',
                   '-fno-omit-frame-pointer','-fsanitize=address,undefined','-I'+str(build)]
            flags+=['-I'+str(p) for p in incdirs]
            dp=build/'lattice_dispatch_probe.c'; dp.write_text(DISPATCH_C)
            qp=build/'lattice_directory_probe.c'; qp.write_text(DIRECTORY_C)
            binaries={}
            for kind,source in (('lbs',SOURCES[0]),('s3',SOURCES[1]),('dynamodb',SOURCES[2])):
                binary=build/('lattice-'+kind)
                must([*flags,'-DLATTICE_SOURCE="'+str(build/source)+'"',
                      '-DLATTICE_LBS='+str(int(kind=='lbs')),str(dp),
                      '-Wl,--gc-sections','-Wl,--wrap=close','-Wl,--wrap=free','-Wl,--wrap=libcperciva_warn','-Wl,--wrap=libcperciva_warnx',
                      '-o',str(binary)], revision+'-compile-'+kind,build)
                binaries[kind]=binary
            for site,kind,idx in (('lbs-done-write','lbs',1),('lbs-done-read','lbs',0),
                                  ('lbs-close','lbs',2),('s3-done','s3',0),('dynamodb-done','dynamodb',0)):
                for mode,name in enumerate(MODES):
                    label=f'{revision}-{site}-{name}'
                    probe(binaries[kind],[mode,idx,0],label,dispatch_expected(revision,site,mode))
                if site.startswith('lbs-done-'):
                    probe(binaries[kind],[0,idx,-1],f'{revision}-{site}-worker-error',
                          dispatch_expected(revision,site,0,worker_error=-1))
            archives=list((build/'liball').glob('*.a'))
            if len(archives)!=1: raise RuntimeError('Expected one liball archive, got '+repr(archives))
            binary=build/'lattice-directory'
            must([*flags,'-DLATTICE_SOURCE="'+str(build/SOURCES[3])+'"',str(qp),
                  '-Wl,--gc-sections','-Wl,--wrap=closedir','-Wl,--wrap=libcperciva_warn','-Wl,--wrap=libcperciva_warnx',str(archives[0]),
                  '-lpthread','-lm','-lssl','-lcrypto','-o',str(binary)],revision+'-compile-directory',build)
            fixture=build/'lattice-fixture'; fixture.mkdir()
            (fixture/'blks_0000000000000000').write_bytes(b'public generated fixture')
            (fixture/'unrelated.txt').write_bytes(b'ignored')
            for mode,name in enumerate(('success','eintr-released','eio-released')):
                guard=revision=='base' and mode==1
                expected={'process_exit':90 if guard else 0,'guard':int(guard),
                          'calls':2 if guard else 1,'warnings':int(mode==2),'fd_open':0}
                if not guard: expected['result_nonnull']=int(mode!=2)
                probe(binary,[mode,fixture],f'{revision}-closedir-{name}',expected)
    failed=[x['case'] for x in RESULTS if not x['pass']]
    summary={'checked_outcomes':len(RESULTS),'matched':len(RESULTS)-len(failed),'failed':failed,
             'portability_caveat':'In the EINTR-retains-fd model, head returns success with target still open; base retries and closes it. This is recorded, not silently certified as correct.',
             'fault_injection_scope':'Real target translation units and descriptors; external worker/netbuf cleanup stubbed; no cloud or full-daemon execution.'}
    (OUT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('SUMMARY '+json.dumps(summary,sort_keys=True),flush=True)
    if failed: raise SystemExit(1)


if __name__=='__main__': main()
