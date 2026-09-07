#!/usr/bin/env python3
"""PR321 exact-object before/after replay with actual writer/deleter threads.

Usage: python3 run.py CHECKOUT_DIRECTORY OUTPUT_DIRECTORY
Both pinned checkouts must be built with ASan/UBSan and debug symbols.
Expected original-revision sanitizer aborts are recorded, not hidden/retried.
"""
from __future__ import annotations
import errno
import hashlib
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys

REVISIONS = {'base': '3de151b5d878b0714552c3658562eb5a87b378ce',
             'head': 'fa4d96f81dfbea69a9a47a52b706ec7a4d38ee87'}
ROOT = Path(sys.argv[1]).resolve()
OUT = Path(sys.argv[2]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
HARNESS = Path(__file__).with_name('observe.c').resolve()
ENV = dict(os.environ, LC_ALL='C',
    ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=0:exitcode=86',
    UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:exitcode=87')
FIRST = 100
cases = []
for n in (1, 2, 4, 8):
    for index, within, boundary in itertools.product(sorted({0, n//2, n-1}), (0, 1), ('unlock', 'path')):
        cases.append(('grow', boundary, n, index, within))
for mode, n in (('delete-no-move',4), ('delete-shift',4), ('delete-shrink',8), ('append-inplace',16)):
    cases += [(mode, boundary, n, index, within) for index, within, boundary in
              itertools.product((0,n-1),(0,1),('unlock','path'))]
cases += [('normal','none',4,index,within) for index,within in itertools.product((0,3),(0,1))]
cases += [('bounds','none',4,index,0) for index in (-1,4)]
for mode in ('path-error','disk-error','latency'):
    cases += [(mode,'none',4,1,within) for within in (0,1)]
assert len(cases) == len(set(cases)) == 80
records, failures, manifest = [], [], {}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(src: Path, *args: str) -> str:
    return subprocess.check_output(['git','-C',str(src),*args], text=True).strip()


def block_bytes(number: int, size: int) -> bytes:
    return bytes((number*31+i*7) % 256 for i in range(size))


def expected_queue(mode: str, n: int) -> dict:
    files, minimum, nextblk = n, FIRST, FIRST+2*n
    if mode == 'grow': files, nextblk = n+1, nextblk+1
    elif mode == 'append-inplace': nextblk += 1
    elif mode == 'delete-no-move': files, minimum = n-1, FIRST+2
    elif mode.startswith('delete-'): files, minimum = 1, FIRST+2*(n-1)
    return {'files_after':files,'minimum_after':minimum,'next_after':nextblk}


def check(revision: str, case: tuple, size: int, process: subprocess.CompletedProcess,
          events: list[dict]) -> list[str]:
    mode, boundary, n, index, within = case
    errors = []
    def expect(actual,wanted,label):
        if actual != wanted: errors.append(f'{label}: expected {wanted!r}, got {actual!r}')
    mutation = mode in ('grow','append-inplace','delete-no-move','delete-shift','delete-shrink')
    expected_uaf = revision=='base' and mode in ('grow','delete-shrink')
    pre = [event for event in events if event.get('event')=='interleave']
    final = [event for event in events if event.get('event')=='result']
    expect(len(pre),int(mutation),'interleave receipt count')
    for event in pre:
        expect(event['joined'],1,'real worker joined')
        for key,value in expected_queue(mode,n).items(): expect(event[key],value,key)
        if mode in ('grow','delete-shrink'):
            if event['moves'] < 1: errors.append('Actual queue allocation did not move')
        else:
            expect(event['moves'],0,'control queue did not reallocate')
            expect(event['reallocations'],0,'control queue did not resize allocation')
    if expected_uaf:
        expect(process.returncode,86,'original ASan exit code')
        expect(len(final),0,'no fabricated normal result after original UAF')
        for text in ('AddressSanitizer: heap-use-after-free','storage_read','READ of size 8'):
            if text not in process.stderr: errors.append('Missing expected sanitizer attribution: '+text)
        return errors
    expect(process.returncode,0,'observer exit')
    if 'AddressSanitizer' in process.stderr or 'runtime error:' in process.stderr:
        errors.append('Unexpected sanitizer diagnostic')
    expect(len(final),1,'final result count')
    if not final: return errors
    row = final[0]
    existing = 0 <= index < n
    deleted = mode.startswith('delete-') and index==0
    shifted = revision=='base' and mode=='delete-shift' and index==0
    rc = 0 if not existing or deleted else -1 if mode in ('path-error','disk-error') else 1
    if shifted and boundary=='unlock': rc = -1
    expect(row['returncode'],rc,'storage_read result')
    expect(row['blocklen'],size,'block size')
    expect(row['data_matches'],int(rc==1),'byte-exact requested block')
    expect(row['appended_block_matches'],1 if mode in ('grow','append-inplace') else -1,'new writer block preserved')
    expect(row['path_calls'],int(existing),'path calls')
    expect(row['disk_read_calls'],int(existing and mode!='path-error'),'disk read calls')
    expect(row['sleep_calls'],int(mode=='latency'),'requested latency calls')
    expect(row['interleaves'],int(mutation),'interleaves')
    expect(row['joined'],int(mutation),'joined worker')
    fnum = FIRST+2*index if existing else (1<<64)-1
    observed_fnum = FIRST+2*(n-1) if shifted and boundary=='unlock' else fnum
    expect(row['expected_fnum'],fnum,'expected file number')
    expect(row['observed_fnum'],observed_fnum,'actual filename argument')
    expected_offset = -1 if not existing or mode=='path-error' else within*size
    if shifted: expected_offset = (2*index+within-2*(n-1))*size
    expect(row['observed_offset'],expected_offset,'actual disk offset argument')
    for key,value in expected_queue(mode,n).items(): expect(row[key],value,key)
    if mode=='disk-error': expect(row['errno'],errno.EIO,'I/O errno preserved')
    if mode=='path-error': expect(row['errno'],errno.ENOMEM,'path error preserved')
    if deleted and not (shifted and boundary=='unlock'): expect(row['errno'],errno.ENOENT,'deleted-file handling')
    return errors


for revision, expected in REVISIONS.items():
    src = ROOT/revision
    if git(src,'rev-parse','HEAD') != expected: raise RuntimeError(f'{revision}: wrong source')
    subprocess.run(['git','-C',str(src),'diff','--exit-code'],check=True)
    binary = OUT/('observe-'+revision)
    command = ['gcc','-std=c99','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
        '-D_POSIX_C_SOURCE=200809L','-D_XOPEN_SOURCE=700']
    for path in ('lbs','lib/proto_lbs','libcperciva/datastruct','libcperciva/util'):
        command.append('-I'+str(src/path))
    command.append(str(HARNESS))
    command += [str(src/'lbs'/name) for name in ('storage.o','storage_findfiles.o','storage_util.o','disk.o')]
    command += [str(src/'liball/liball.a'),str(src/'liball/optional_mutex_pthread/liball_optional_mutex_pthread.a'),
                '-lcrypto','-lpthread','-o',str(binary)]
    for symbol in ('storage_util_unlock','storage_util_mkpath','elasticqueue_get','realloc','disk_read','nanosleep'):
        command.append('-Wl,--wrap='+symbol)
    build = subprocess.run(command,capture_output=True,text=True,timeout=120,env=ENV)
    (OUT/('observer-build-'+revision+'.json')).write_text(json.dumps(
        {'command':command,'returncode':build.returncode,'stdout':build.stdout,'stderr':build.stderr},indent=2)+'\n')
    if build.returncode:
        print(build.stderr,flush=True)
        raise RuntimeError(f'{revision}: observer compile failed')
    size = int(subprocess.check_output([str(binary),'--blocklen'],text=True,env=ENV))
    manifest[revision] = {'commit':expected,'storage_blob':git(src,'rev-parse','HEAD:lbs/storage.c'),
        'storage_object_sha256':sha256(src/'lbs/storage.o'),'lbs_binary_sha256':sha256(src/'lbs/lbs'),
        'liball_sha256':sha256(src/'liball/liball.a'),'observer_sha256':sha256(binary),'blocklen':size}
    for case in cases:
        mode,boundary,n,index,within = case
        name = f'{mode}-{boundary}-n{n}-i{index}-b{within}'
        directory = OUT/'fixtures'/revision/name
        directory.mkdir(parents=True)
        for f in range(n):
            start = FIRST+2*f
            (directory/f'blks_{start:016x}').write_bytes(block_bytes(start,size)+block_bytes(start+1,size))
        result = subprocess.run([str(binary),mode,boundary,str(n),str(index),str(within),str(directory)],
                                capture_output=True,text=True,timeout=12,env=ENV)
        try: events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        except json.JSONDecodeError: events = []
        problems = check(revision,case,size,result,events)
        row = {'revision':revision,'case':name,'mode':mode,'boundary':boundary,'files':n,
            'selected_index':index,'within_file':within,'returncode':result.returncode,
            'expected_original_uaf':revision=='base' and mode in ('grow','delete-shrink'),
            'stdout':result.stdout,'stderr':result.stderr,'events':events,'expectation_passed':not problems}
        records.append(row)
        if problems: failures.append({'revision':revision,'case':name,'errors':problems,'row':row})
        print('CASE',json.dumps({k:row[k] for k in ('revision','case','returncode','expected_original_uaf','expectation_passed')}),flush=True)
    subprocess.run(['git','-C',str(src),'diff','--exit-code'],check=True)
summary = {'cases':len(records),'expected_outcomes_passed':sum(r['expectation_passed'] for r in records),
    'failed_expectations':len(failures),'source':manifest,'observer_source_sha256':sha256(HARNESS),
    'original_heap_uaf_reports':sum(r['revision']=='base' and 'AddressSanitizer: heap-use-after-free' in r['stderr'] for r in records),
    'head_sanitizer_diagnostics':sum(r['revision']=='head' and ('AddressSanitizer' in r['stderr'] or 'runtime error:' in r['stderr']) for r in records),
    'limits':['Actual storage/queue/disk objects, real temporary files and joined writer/deleter threads; deliberate boundary scheduling',
        'ASan/UBSan instrumented GCC -O1; no every-interleaving, production daemon or cross-platform guarantee',
        'Expected original UAFs abort the subprocess; no post-fault fabricated cleanup or result',
        'Missing-file races remain supported outcomes, not a claim that copied file numbers prevent deletion',
        'No full native test suite, remote target, exploitability or bounty-impact certification']}
(OUT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(OUT/'matrix.json').write_text(json.dumps(records,indent=2)+'\n')
(OUT/'failures.json').write_text(json.dumps(failures,indent=2)+'\n')
print('SUMMARY',json.dumps(summary),flush=True)
for failure in failures: print('FAIL',json.dumps(failure),flush=True)
raise SystemExit(1 if failures else 0)
