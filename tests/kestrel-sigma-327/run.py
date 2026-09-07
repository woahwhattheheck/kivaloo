#!/usr/bin/env python3
"""Exact-source callback accounting replay for existing kivaloo PR327.

Network access is restricted to public GitHub source fetches. No S3 service,
credentials, production data, daemon, or natural-failure claim is involved.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import signal
import subprocess
import sys

BASE = '3de151b5d878b0714552c3658562eb5a87b378ce'
HEAD = '3c1d3a60ccc190b04d10daee5d709bd0ee26ef0e'
HEAD_BLOB = '1beab0077c5891760334fae1ddbb0bf82994a17c'
CASES = [(name, 0) for name in (
    'append_failed', 'append_ok', 'append_callback_error',
    'append_enqueue_fail', 'append_alloc_fail', 'mixed_completion',
    'get_ok', 'get_failed', 'get_short', 'get_callback_error',
    'get_enqueue_fail', 'get_alloc_fail', 'failed_append_teardown')]
CASES += [(name, pending) for name in ('append_failed', 'mixed_completion') for pending in (1, 9)]


def execute(args, cwd, path, env, timeout=90):
    print('$ ' + shlex.join(str(x) for x in args), flush=True)
    with path.open('wb') as stream:
        p = subprocess.Popen(args, cwd=cwd, env=env, stdout=stream,
                             stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            raise RuntimeError(f'timeout: {args[0]}') from None
    text = path.read_text(errors='replace')
    print(f'exit={code} log={path.name}', flush=True)
    return code, text


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: run.py NEW_OUTPUT_DIRECTORY')
    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=False)
    logs = output / 'logs'; logs.mkdir()
    harness = Path(__file__).with_name('accounting.c').resolve()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = dict(os.environ, GIT_TERMINAL_PROMPT='0',
               ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    report = {'base': BASE, 'head': HEAD, 'cases': [], 'sources': {},
              'harness_sha256': hashlib.sha256(harness.read_bytes()).hexdigest(),
              'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'scope': 'Exact included production translation unit; queued callback observations, not a live S3 daemon or full make test. The real daemon exits on this error path.'}
    try:
        execute(['sh', '-c', 'uname -a; cc --version; cat /etc/os-release'], output,
                logs / 'environment.log', env)
        for label, sha, fixed, repo in [('base', BASE, 0, 'Tarsnap/kivaloo'),
                                       ('head', HEAD, 1, 'woahwhattheheck/kivaloo')]:
            source = output / (label + '-source'); source.mkdir()
            for step, command in (
                ('init', ['git', 'init', '-q']),
                ('fetch', ['git', 'fetch', '--depth=1', f'https://github.com/{repo}.git', sha]),
                ('checkout', ['git', 'checkout', '--detach', 'FETCH_HEAD']),
            ):
                code, text = execute(command, source, logs / f'{label}-{step}.log', env)
                assert code == 0, text[-6000:]
            assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip() == sha
            data = (source / 'lbs-s3/s3state.c').read_bytes()
            blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            assert blob == subprocess.check_output(['git', 'rev-parse', 'HEAD:lbs-s3/s3state.c'], cwd=source, text=True).strip()
            if fixed: assert blob == HEAD_BLOB
            report['sources'][label] = {'git_blob': blob, 'sha256': hashlib.sha256(data).hexdigest()}
            binary = output / (label + '-accounting')
            command = ['cc', '-std=c99', '-D_POSIX_C_SOURCE=200809L', '-g', '-O1',
                       '-Wall', '-Wextra', '-Werror', '-pedantic',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections']
            includes = [source / 'lbs-s3'] + sorted({p.parent for p in source.rglob('*.h') if '.git' not in p.parts})
            for inc in includes: command += ['-I', str(inc)]
            command += [str(harness), '-Wl,--gc-sections', '-o', str(binary)]
            code, text = execute(command, source, logs / f'{label}-compile.log', env)
            assert code == 0, text
            for case, pending in CASES:
                case_id = f'{case}-{pending}'
                code, text = execute([str(binary), case, str(fixed), str(pending)], source,
                                     logs / f'{label}-{case_id}.log', env, timeout=15)
                assert not any(x in text for x in ('ERROR: AddressSanitizer', 'LeakSanitizer', 'runtime error:')), text
                if case == 'failed_append_teardown' and not fixed:
                    assert code == -signal.SIGABRT and 'S->npending == 0' in text, (code, text)
                    observed = {'expected_assertion': 'S->npending == 0', 'returncode': code}
                else:
                    assert code == 0, (case_id, code, text)
                    lines = [line for line in text.splitlines() if line.startswith('{')]
                    assert len(lines) == 1, text
                    observed = json.loads(lines[0])
                    expected = pending + (1 if not fixed and case in ('append_failed', 'mixed_completion') else 0)
                    assert observed['pending'] == expected
                report['cases'].append({'revision': label, 'case': case_id, 'observed': observed, 'matched': True})
            code, text = execute(['git', 'diff', '--exit-code', 'HEAD', '--', 'lbs-s3/s3state.c'],
                                 source, logs / f'{label}-unchanged.log', env)
            assert code == 0, text
        assert len(report['cases']) == 34
        base = {r['case']: r['observed'] for r in report['cases'] if r['revision'] == 'base'}
        head = {r['case']: r['observed'] for r in report['cases'] if r['revision'] == 'head'}
        report['changed_cases'] = [name for name in base if base[name] != head[name]]
        report['unchanged_controls'] = [name for name in base if base[name] == head[name]]
        assert len(report['changed_cases']) == 7 and len(report['unchanged_controls']) == 10
        report['success'] = True
        print('PASS: 34 expected exact-source outcomes; 7 changed cases and 10 unchanged controls.', flush=True)
    except BaseException as exc:
        report['success'] = False
        report['failure'] = f'{type(exc).__name__}: {exc}'
        raise
    finally:
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        sums = {str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(logs.glob('*.log'))}
        (output / 'log-sha256.json').write_text(json.dumps(sums, indent=2) + '\n')


if __name__ == '__main__':
    main()
