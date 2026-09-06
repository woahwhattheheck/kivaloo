#!/usr/bin/env python3
"""Exact-source, real-pool callback regressions for Tarsnap/kivaloo #335.

Usage: python3 run.py /path/to/base-checkout /path/to/head-checkout
No sources are rewritten. Each process is bounded and its logs are retained.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
COMMITS = {'base': '3de151b5d878b0714552c3658562eb5a87b378ce',
           'head': '8507e3201015af50d978a834ac70cebbaba751ab'}
TARGET_BLOBS = {'base': 'f99dff5ee5e8f49694684921675d11b580c718b6',
                'head': '99854f1d3ae864980a488542126483bff5ee5b03'}
DEPENDENCIES = {'lib/datastruct/pool.c': 'd54c90db9ee9011df9d7656222f827a51bbae474',
                'lib/datastruct/pool.h': 'b8822edb804bb7d4447b1a00c18926069f6a3917',
                'kvlds/node.h': '7e6d2f488ce648a6ea6bef43d9c39d4b2e4127a8',
                'kvlds/btree_node.h': 'fa91d9bcbf17ef9ff334ed1eee1ca1c76d282ae1'}
# Expected invariant masks on the original source: COUNT=1, GROUP=2,
# LINKS=4, LOCK=8, STATUS=16, RECORD=32, POSSIBLE=64. All head masks are 0.
CASES = [
    ('clean', 'C', 0, 0), ('dirty', 'D', 0, 70), ('shadow', 'S', 0, 70),
    ('allocation-failure', 'F', 0, 71), ('all-dirty', 'DD', 0, 70),
    ('shadow-dirty', 'SD', 0, 70), ('dirty-clean', 'DC', 0, 0),
    ('clean-dirty', 'CD', 0, 0), ('failure-dirty', 'FD', 0, 71),
    ('dirty-failure', 'DF', 0, 71), ('failure-clean', 'FC', 0, 1),
    ('clean-failure', 'CF', 0, 1), ('two-clean', 'CC', 0, 0),
    ('three-clean', 'CCC', 0, 0), ('unlink-head', 'D', 1, 70),
    ('unlink-middle', 'D', 2, 70), ('unlink-tail', 'D', 3, 70),
    ('failure-middle', 'F', 2, 71), ('two-failures-dirty', 'FFD', 0, 71),
    ('two-clean-dirty-failure', 'CCDF', 0, 1),
    ('clean-shadow-dirty', 'CSD', 0, 0), ('dirty-clean-shadow', 'DCS', 0, 0),
]


def verify_blob(path: Path, expected: str) -> None:
    data = path.read_bytes()
    actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    if actual != expected:
        raise SystemExit(f'Blob mismatch: {path}: {actual} != {expected}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('head', type=Path)
    args = parser.parse_args()
    logs = ROOT / 'logs'
    logs.mkdir(exist_ok=True)
    cc = os.environ.get('CC', 'cc')
    report = {'scope': 'actual callback/free helpers + real pool, isolated leaf/list fixtures',
              'platform': platform.platform(),
              'compiler': subprocess.check_output([cc, '--version'], text=True).splitlines()[0],
              'commits': COMMITS, 'target_blobs': TARGET_BLOBS,
              'dependency_blobs': DEPENDENCIES, 'builds': [], 'results': []}
    try:
        with tempfile.TemporaryDirectory(prefix='keel-kivaloo-335-') as tmp:
            for stage in ('base', 'head'):
                target = getattr(args, stage).resolve()
                actual_ref = subprocess.check_output(['git', '-C', str(target), 'rev-parse', 'HEAD'], text=True).strip()
                if actual_ref != COMMITS[stage]:
                    raise SystemExit(f'Commit mismatch: {stage}: {actual_ref}')
                verify_blob(target / 'kvlds/btree_cleaning.c', TARGET_BLOBS[stage])
                for name, blob in DEPENDENCIES.items():
                    verify_blob(target / name, blob)
                exe = Path(tmp) / stage
                cmd = [cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-D_XOPEN_SOURCE=700',
                       '-Wall', '-Wextra', '-Werror', '-pedantic', '-O1', '-g',
                       '-fno-omit-frame-pointer', '-fno-builtin-malloc', '-fno-builtin-free',
                       '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined']
                for include in ['kvlds', 'lib/datastruct', 'libcperciva/events', 'libcperciva/util']:
                    cmd += ['-I', str(target / include)]
                cmd += [str(ROOT / 'cleaner_harness.c'), str(target / 'lib/datastruct/pool.c'),
                        '-Wl,--gc-sections', '-Wl,--wrap=malloc', '-Wl,--wrap=free', '-o', str(exe)]
                proc = subprocess.run(cmd, capture_output=True, text=True, timeout=45)
                (logs / f'{stage}-compile.txt').write_text(proc.stdout + proc.stderr)
                report['builds'].append({'stage': stage, 'command': cmd,
                                         'returncode': proc.returncode, 'diagnostics': proc.stdout + proc.stderr})
                if proc.returncode:
                    raise SystemExit(proc.stderr)
                for name, ops, position, original_mask in CASES:
                    expected = original_mask if stage == 'base' else 0
                    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:exitcode=86:color=never',
                               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
                    proc = subprocess.run([str(exe), ops, str(position)], text=True,
                                          capture_output=True, timeout=5, env=env)
                    data = None
                    try:
                        data = json.loads(proc.stdout)
                    except json.JSONDecodeError:
                        pass
                    matched = (proc.returncode == (2 if expected else 0) and data is not None
                               and data['failure_mask'] == expected
                               and 'AddressSanitizer' not in proc.stderr and 'runtime error:' not in proc.stderr)
                    log = f'{stage}-{name}.txt'
                    (logs / log).write_text(f'returncode={proc.returncode}\n{proc.stdout}{proc.stderr}')
                    report['results'].append({'stage': stage, 'case': name, 'expected_mask': expected,
                                              'returncode': proc.returncode, 'observed': data,
                                              'matched': matched, 'log': 'logs/' + log,
                                              'stdout': proc.stdout, 'stderr': proc.stderr})
                    print(stage, name, 'expected_mask=' + str(expected), 'MATCH=' + str(matched), flush=True)
    finally:
        report['matched'] = sum(row['matched'] for row in report['results'])
        report['total'] = len(report['results'])
        (ROOT / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"RESULT: {report['matched']}/{report['total']} expected outcomes")
    if report['total'] != 2 * len(CASES) or report['matched'] != report['total']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
