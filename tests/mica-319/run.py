#!/usr/bin/env python3
"""PR319 expected-outcome replay using actual disk.o and temporary files.

Usage: python3 run.py CHECKOUT_DIRECTORY OUTPUT_DIRECTORY
Both base/ and head/ must be normally built at the pinned revisions below.
A passing replay means its predictions matched, NOT that both descriptor-close
contracts are repaired. The retained-EINTR cases explicitly expose a head leak.
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
             'head': '44b96da661657acce648dfa22dd396b307c516f0'}
OPS = ('syncdir', 'read', 'read-zero', 'create', 'create-nosync', 'append', 'append-nosync')
CLOSES = ('normal', 'reuse-eintr', 'released-eintr', 'retained-eintr',
          'released-eio', 'released-enospc', 'released-einprogress')
ROOT = Path(sys.argv[1]).resolve()
OUT = Path(sys.argv[2]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
HARNESS = Path(__file__).with_name('observe.c').resolve()
ENV = dict(os.environ, LC_ALL='C')
INITIAL = bytes((i*5+11) % 256 for i in range(128))
WRITTEN = bytes((i*17+3) % 256 for i in range(32))
UNTOUCHED = bytes([0xa5])*32
cases = [(op, close, 'none') for op, close in itertools.product(OPS, CLOSES)]
cases += [(op, 'normal', f) for op, f in itertools.product(OPS, ('open-eintr', 'open-eacces'))]
cases += [('read', 'normal', f) for f in ('seek-eio', 'io-eio', 'io-eintr', 'io-eof', 'partial', 'missing')]
cases += [('read-zero', 'normal', f) for f in ('seek-eio', 'missing')]
cases += [('syncdir', 'normal', f) for f in ('fsync-eio', 'fsync-eintr')]
for op in OPS[3:]:
    cases += [(op, 'normal', f) for f in ('io-eio', 'io-eintr', 'partial', 'fsync-eio', 'fsync-eintr')]
cases += [(op, 'normal', 'exists') for op in ('create', 'create-nosync')]
cleanup_faults = [('syncdir', 'fsync-eio')]
cleanup_faults += [('read', f) for f in ('seek-eio', 'io-eio', 'io-eof')]
cleanup_faults += [(op, 'io-eio') for op in OPS[3:]]
cleanup_faults += [(op, 'fsync-eio') for op in ('create', 'append')]
cases += [(op, close, f) for (op, f), close in itertools.product(cleanup_faults,
          ('reuse-eintr', 'released-eintr', 'retained-eintr'))]
assert len(cases) == len(set(cases)) == 125
records, failures, manifest = [], [], {}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(src: Path, *args: str) -> str:
    return subprocess.check_output(['git', '-C', str(src), *args], text=True).strip()


def check(revision: str, op: str, close: str, fault: str, observed: dict,
          target: Path, stderr: str) -> list[str]:
    errors = []
    def expect(actual, wanted, label):
        if actual != wanted:
            errors.append(f'{label}: expected {wanted!r}, got {actual!r}')
    opening_failed = fault in ('open-eacces', 'missing', 'exists')
    needs_sync = op in ('syncdir', 'create', 'append')
    early_failed = opening_failed or fault in ('seek-eio', 'io-eio', 'io-eof') or (fault == 'fsync-eio' and needs_sync)
    retry = revision == 'base' and not early_failed and close in ('reuse-eintr', 'released-eintr', 'retained-eintr')
    expected_rc = -1 if early_failed or close in ('released-eio', 'released-enospc', 'released-einprogress') or (revision == 'base' and close == 'released-eintr') else 0
    identity = 0
    if not opening_failed and not retry:
        if close == 'retained-eintr': identity = 1
        if close == 'reuse-eintr': identity = 2
    values = {'returncode': expected_rc, 'target_opened': int(not opening_failed),
        'open_calls': 2 if fault == 'open-eintr' else 1,
        'close_calls': 0 if opening_failed else 2 if retry else 1,
        'close_injected': int(not opening_failed and close != 'normal'),
        'descriptor_identity': identity,
        'reused_by_second_thread': int(not opening_failed and close == 'reuse-eintr'),
        'replacement_readable': int(identity == 2), 'replacement_writable': int(identity == 2),
        'joined': int(not opening_failed and close == 'reuse-eintr'),
        'cleanup_closes': int(identity != 0)}
    for key, value in values.items(): expect(observed[key], value, key)
    reached_io = not opening_failed and fault != 'seek-eio'
    expected_io_calls = 0
    if reached_io and (op == 'read' or op in OPS[3:]):
        expected_io_calls = 2 if fault == 'io-eintr' else 5 if fault == 'partial' else 1
    expect(observed['read_calls'], expected_io_calls if op == 'read' else 0, 'read calls')
    expect(observed['write_calls'], expected_io_calls if op in OPS[3:] else 0, 'write calls')
    expected_sync = 0
    if not opening_failed and needs_sync and fault != 'io-eio':
        expected_sync = 2 if fault == 'fsync-eintr' else 1
    expect(observed['fsync_calls'], expected_sync, 'fsync calls')
    expect(observed['seek_calls'], int(not opening_failed and op.startswith('read')), 'seek calls')
    data_read = op == 'read' and reached_io and fault not in ('io-eio', 'io-eof')
    expect(observed['read_buffer'], (INITIAL[7:39] if data_read else UNTOUCHED).hex(), 'read buffer content')
    if op in OPS[3:]:
        creating = op.startswith('create')
        existed = not creating or fault == 'exists'
        if opening_failed:
            expect(target.exists(), existed, 'file existence after failed open')
            if existed: expect(target.read_bytes(), INITIAL, 'existing file unchanged')
        else:
            content = (b'' if creating else INITIAL) + (b'' if fault == 'io-eio' else WRITTEN)
            expect(target.read_bytes(), content, 'exact file contents')
            if creating: expect(target.stat().st_mode & 0o777, 0o600, 'created file mode')
    if fault == 'missing':
        expect(observed['errno'], errno.ENOENT, 'missing-file errno')
        expect(stderr, '', 'missing disk_read is silent')
    if close in ('released-eio', 'released-enospc', 'released-einprogress') and not early_failed:
        if 'close(' not in stderr: errors.append('Lost deferred close error diagnostic')
    if fault in ('open-eintr','io-eintr','fsync-eintr') and not (fault.startswith('fsync') and not needs_sync):
        expect(observed['fault_fired'], 1, 'single interrupted-call injection')
    return errors


for revision, expected in REVISIONS.items():
    src = ROOT/revision
    if git(src, 'rev-parse', 'HEAD') != expected: raise RuntimeError(f'{revision}: incorrect source')
    subprocess.run(['git', '-C', str(src), 'diff', '--exit-code'], check=True)
    binary = OUT/('observe-'+revision)
    command = ['gcc', '-std=c99', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-D_POSIX_C_SOURCE=200809L', '-D_XOPEN_SOURCE=700', '-I'+str(src/'lbs'),
        str(HARNESS), str(src/'lbs/disk.o'), str(src/'liball/liball.a'),
        str(src/'liball/optional_mutex_pthread/liball_optional_mutex_pthread.a'),
        '-lcrypto', '-lpthread', '-o', str(binary)]
    for symbol in ('open', 'close', 'read', 'write', 'fsync', 'lseek'):
        command.append('-Wl,--wrap='+symbol)
    build = subprocess.run(command, capture_output=True, text=True, timeout=120, env=ENV)
    (OUT/('observer-build-'+revision+'.json')).write_text(json.dumps(
        {'command': command, 'returncode': build.returncode, 'stdout': build.stdout,
         'stderr': build.stderr}, indent=2)+'\n')
    if build.returncode:
        print(build.stderr, flush=True)
        raise RuntimeError(f'{revision}: observer failed to link')
    manifest[revision] = {'commit': expected, 'disk_blob': git(src, 'rev-parse', 'HEAD:lbs/disk.c'),
        'disk_object_sha256': sha256(src/'lbs/disk.o'), 'lbs_binary_sha256': sha256(src/'lbs/lbs'),
        'liball_sha256': sha256(src/'liball/liball.a'), 'observer_sha256': sha256(binary)}
    for op, close, fault in cases:
        name = f'{op}-{close}-{fault}'
        directory = OUT/'fixtures'/revision/name
        directory.mkdir(parents=True)
        target = directory if op == 'syncdir' else directory/'data.bin'
        sentinel = directory/'sentinel.bin'
        sentinel.write_bytes(b'MICA unrelated-descriptor sentinel\n')
        if op != 'syncdir' and (not op.startswith('create') or fault == 'exists') and fault != 'missing':
            target.write_bytes(INITIAL)
        result = subprocess.run([str(binary), op, close, fault, str(target), str(sentinel)],
                                capture_output=True, text=True, timeout=10, env=ENV)
        row = {'revision': revision, 'case': name, 'operation': op, 'close_contract': close,
               'io_fault': fault, 'returncode': result.returncode,
               'stdout': result.stdout, 'stderr': result.stderr}
        try: observed = json.loads(result.stdout) if result.returncode == 0 else None
        except json.JSONDecodeError: observed = None
        row['observed'] = observed
        problems = check(revision, op, close, fault, observed, target, result.stderr) if observed else ['Observer failed']
        row['expectation_passed'] = not problems
        records.append(row)
        if problems: failures.append({'revision': revision, 'case': name, 'errors': problems, 'row': row})
        print('CASE', json.dumps(row), flush=True)
    subprocess.run(['git', '-C', str(src), 'diff', '--exit-code'], check=True)
summary = {'cases': len(records), 'expected_outcomes_passed': sum(r['expectation_passed'] for r in records),
    'failed_expectations': len(failures), 'source': manifest, 'observer_source_sha256': sha256(HARNESS),
    'interpretation': ['Linux release/reuse success paths are repaired by the submitted head',
        'Retained-descriptor EINTR success paths are NOT repaired: head reports success while descriptor remains open',
        'Pre-existing error cleanup is one-shot on both revisions and retains the same portability limitation',
        'Passing expected-outcome checks is not a universal portability approval'],
    'limits': ['Real normally built disk.o, supporting library, files, I/O and second-thread descriptor reuse',
        'Return/error semantics are deliberately injected; not natural kernel EINTR or another-OS execution',
        'Only temporary synthetic fixtures; live test-owned descriptors cleaned after state is recorded',
        'No full native suite, sanitizer, platform-wide or bounty-impact certification']}
(OUT/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
(OUT/'matrix.json').write_text(json.dumps(records, indent=2)+'\n')
(OUT/'failures.json').write_text(json.dumps(failures, indent=2)+'\n')
print('SUMMARY', json.dumps(summary), flush=True)
for failure in failures: print('FAIL', json.dumps(failure), flush=True)
raise SystemExit(1 if failures else 0)
