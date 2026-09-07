#!/usr/bin/env python3
"""Bounded exact-revision builds and injected rollback tests for kivaloo #329.

No daemon, remote storage, credentials, or source modifications are used.
The C observer includes the actual worker and dispatcher translation units.
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
import time

BASE = "3de151b5d878b0714552c3658562eb5a87b378ce"
HEAD = "123e6e4e88175d845cdfaecd68ff60d0be9f7d2a"
HEAD_BLOBS = {
    "lbs/worker.c": "b5ceef7274606f09bc2d6e9f9b4274f83c51bbd4",
    "lbs/dispatch_request.c": "77bafba6216f97a01495e98dd343953577d99a2c",
}
WORKER_CASES = ("worker_lock_fail", "worker_signal_fail", "worker_success",
                "worker_consume_failed", "worker_retry")
DISPATCH_CASES = (
    "append_signal_fail", "append_lock_fail", "append_success",
    "append_busy_response_fail", "append_busy_response_ok",
    "append_nextblock_fail_active", "append_nextblock_fail_idle",
    "append_wrongblock_response_fail", "append_wrongblock_response_ok",
    "free_signal_fail", "free_lock_fail", "free_success_idle",
    "free_response_fail_after_assign", "free_busy_response_ok",
    "free_busy_response_fail", "read_signal_fail", "read_lock_fail",
    "read_success", "read_alloc_fail",
)


def execute(args: list[str], cwd: Path, log: Path, *, timeout: int = 30,
            env: dict[str, str] | None = None, check: bool = True) -> tuple[int, str]:
    print("$ " + shlex.join(args), flush=True)
    start = time.monotonic()
    proc = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True)
    try:
        output, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        output, _ = proc.communicate()
        log.write_bytes(output)
        raise RuntimeError(f"timeout after {timeout}s: {args[0]}") from None
    log.write_bytes(output)
    text = output.decode("utf-8", "replace")
    print(f"  exit={proc.returncode} elapsed={time.monotonic()-start:.3f}s log={log.name}", flush=True)
    if check and proc.returncode != 0:
        print(text[-12000:], flush=True)
        raise RuntimeError(f"command failed: {shlex.join(args)}")
    return proc.returncode, text


def expected_worker(name: str, op: int, fixed: bool) -> dict[str, int]:
    lock = name == "worker_lock_fail"
    consume = name == "worker_consume_failed"
    retry = name == "worker_retry"
    failed_signal = name in ("worker_signal_fail", "worker_consume_failed", "worker_retry")
    before = 0 if lock or (fixed and failed_signal) else 1
    return {
        "ret": -1 if lock or (failed_signal and not retry) else 0,
        "haswork_before": before, "workdone_before": 7 if lock else 0,
        "haswork": 1 if retry else before,
        "workdone": 7 if lock else (1 if consume and not fixed else 0),
        "op": 99 if lock else op,
        "blkno": 111 if lock else (8 if retry else 7),
        "reqID": 333 if lock else (102 if retry else 101),
        "signal_calls": 0 if lock else (2 if retry else 1),
        "wait_calls": int(consume and fixed),
        "operation_calls": int(consume and not fixed),
    }


def expected_dispatch(name: str, fixed: bool) -> dict[str, int]:
    result = dict(ret=0, haswork=1, writer_busy=0, deleter_busy=0,
                  npending=4, signal_calls=1, response_calls=0,
                  request_frees=1, buffer_frees=0, nreaders_idle=0,
                  readq_retained=0, allocation_calls=0)
    if name.startswith("append_"):
        result["writer_busy"] = 1
        if name == "append_signal_fail":
            result.update(ret=-1, haswork=int(not fixed), writer_busy=int(not fixed), buffer_frees=1)
        elif name == "append_lock_fail":
            result.update(ret=-1, haswork=0, writer_busy=int(not fixed), signal_calls=0, buffer_frees=1)
        elif "nextblock_fail" in name:
            active = int(name.endswith("_active"))
            result.update(ret=-1, haswork=active, writer_busy=active, signal_calls=0, buffer_frees=1)
        elif "busy" in name or "wrongblock" in name:
            active = int("busy" in name)
            result.update(ret=-1 if "response_fail" in name else 0,
                          haswork=active, writer_busy=active, signal_calls=0,
                          response_calls=1, npending=3, buffer_frees=1)
    elif name.startswith("free_"):
        result.update(deleter_busy=1, npending=3, response_calls=1)
        if name == "free_signal_fail":
            result.update(ret=-1, haswork=int(not fixed), deleter_busy=int(not fixed),
                          npending=4, response_calls=0)
        elif name == "free_lock_fail":
            result.update(ret=-1, haswork=0, deleter_busy=int(not fixed),
                          npending=4, signal_calls=0, response_calls=0)
        else:
            if "busy" in name:
                result["signal_calls"] = 0
            if "response_fail" in name:
                result["ret"] = -1
    else:
        assert name.startswith("read_")
        result["allocation_calls"] = 1
        if name != "read_success":
            result.update(ret=-1, haswork=int(name == "read_signal_fail" and not fixed),
                          signal_calls=int(name == "read_signal_fail"), request_frees=0,
                          buffer_frees=int(name != "read_alloc_fail"),
                          nreaders_idle=1, readq_retained=1)
    return result


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: run.py NEW_OUTPUT_DIRECTORY")
    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=False)
    logs = output / "logs"
    logs.mkdir()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    environment = dict(os.environ, GIT_TERMINAL_PROMPT="0",
                       ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    harness = Path(__file__).with_name("rollback_harness.c").resolve()
    manifest: dict[str, object] = {
        "base": BASE, "head": HEAD,
        "harness_sha256": hashlib.sha256(harness.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "scenarios": [], "sources": {},
        "limits": "Injected errors and synchronous worker observer; no real scheduler race, running database, live cloud storage or natural pthread-failure claim.",
    }
    (output / "environment.txt").write_text(subprocess.check_output(
        ["sh", "-c", "uname -a; cc --version; make --version; cat /etc/os-release"],
        text=True))
    try:
        for label, revision, fixed, repo in (
            ("base", BASE, False, "Tarsnap/kivaloo"),
            ("head", HEAD, True, "woahwhattheheck/kivaloo"),
        ):
            source = output / (label + "-source")
            source.mkdir()
            execute(["git", "init", "-q"], source, logs / f"{label}-init.log")
            execute(["git", "fetch", "--depth=1", f"https://github.com/{repo}.git", revision],
                    source, logs / f"{label}-fetch.log", timeout=90, env=environment)
            execute(["git", "checkout", "--detach", "FETCH_HEAD"], source, logs / f"{label}-checkout.log")
            actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
            assert actual == revision, (actual, revision)
            blobs = {}
            for path in HEAD_BLOBS:
                data = (source / path).read_bytes()
                blob = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
                object_sha = subprocess.check_output(["git", "rev-parse", "HEAD:" + path], cwd=source, text=True).strip()
                assert blob == object_sha
                if fixed:
                    assert blob == HEAD_BLOBS[path]
                blobs[path] = {"git_blob": blob, "sha256": hashlib.sha256(data).hexdigest()}
            manifest["sources"][label] = blobs
            execute(["make", "-j2"], source, logs / f"{label}-build.log", timeout=300, env=environment)
            includes = [source, source / "lbs"] + sorted({p.parent for p in source.rglob("*.h") if ".git" not in p.parts})
            binary = output / (label + "-rollback")
            compile_args = ["cc", "-std=c99", "-D_POSIX_C_SOURCE=200809L", "-D_XOPEN_SOURCE=700",
                            "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-fno-pie", "-no-pie", "-ffunction-sections", "-fdata-sections"]
            for directory in includes:
                compile_args.extend(["-I", str(directory)])
            compile_args.extend([str(harness), str(source / "libcperciva/util/warnp.c"),
                                 "-pthread", "-Wl,--gc-sections", "-o", str(binary)])
            execute(compile_args, source, logs / f"{label}-compile.log", timeout=60, env=environment)
            cases = [(name, op) for name in WORKER_CASES for op in range(3)]
            cases.extend((name, None) for name in DISPATCH_CASES)
            assert len(cases) == 34
            for name, op in cases:
                case_id = name + ("_" + str(op) if op is not None else "")
                command = [str(binary), name] + ([str(op)] if op is not None else [])
                code, text = execute(command, source, logs / f"{label}-{case_id}.log",
                                     timeout=15, env=environment, check=False)
                assert not any(s in text for s in ("ERROR: AddressSanitizer", "LeakSanitizer", "runtime error:")), text
                if name == "worker_retry" and not fixed:
                    assert code == -signal.SIGABRT and "ctl->haswork == 0" in text, (code, text)
                    observed: object = {"expected_assertion": "ctl->haswork == 0", "returncode": code}
                else:
                    assert code == 0, (label, case_id, code, text)
                    rows = [line for line in text.splitlines() if line.startswith("{")]
                    assert len(rows) == 1, text
                    observed = json.loads(rows[0])
                    expected = expected_worker(name, op, fixed) if op is not None else expected_dispatch(name, fixed)
                    assert observed == expected, (label, case_id, observed, expected)
                manifest["scenarios"].append({"revision": label, "case": case_id, "observed": observed, "matched": True})
                (output / "results.json").write_text(json.dumps(manifest, indent=2) + "\n")
            execute(["git", "diff", "--exit-code", "HEAD", "--", "lbs/worker.c", "lbs/dispatch_request.c"],
                    source, logs / f"{label}-unchanged.log")
        assert len(manifest["scenarios"]) == 68
        manifest["success"] = True
        print("PASS: 2 exact full builds; 68/68 expected before/after scenario outcomes.", flush=True)
    except BaseException as exc:
        manifest["success"] = False
        manifest["failure"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        (output / "results.json").write_text(json.dumps(manifest, indent=2) + "\n")
        checksums = {str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in sorted(logs.glob("*.log"))}
        (output / "log-sha256.json").write_text(json.dumps(checksums, indent=2) + "\n")


if __name__ == "__main__":
    main()
