#!/usr/bin/env python3
"""Bounded Linux Unix-socket regression for kivaloo #331, using real lbs daemons.

No external listener, credentials, cloud service, source instrumentation, or
synthetic application response is used. Every started daemon is tracked and
reaped. All storage and sockets live in newly created temporary directories.
"""
from __future__ import annotations
import argparse
from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

BASE = '3de151b5d878b0714552c3658562eb5a87b378ce'
HEAD = '690f3805fc4dc3e97234478f6eb1aed9d3d163e5'
DISPATCH = {'base': '55f495518684a3a6c85993eb5ac93c88152f42ba',
            'head': '420a06e0a96db7ed7e18fc7c4052201c9c8fb5b3'}
COMMON = {
    'lib/proto_lbs/proto_lbs_server.c': '5200caec16b9ed7f5b08e5d9892bcd8a6eed9374',
    'lib/proto_lbs/proto_lbs.h': '15eb2dd5c3cb7702f23bbf22b2edd23af310d7fb',
    'lib/wire/wire_writepacket.c': 'cdfa87364afa93924a3554cdc17213ef40732a0e',
    'libcperciva/alg/crc32c.c': '7718ad596dbb263001d860ae3363c4507bc1b64c',
    'libcperciva/util/daemonize.c': 'bc7da069c8395e544ddd791e2d6f8575ed308abe',
}

class IdleTimeout(Exception):
    pass


def crc32c(data: bytes) -> bytes:
    # libcperciva uses the CRC of an implicit leading 1 bit, not the common
    # all-ones initialization/final XOR. Its own hello-world vector is checked.
    state = 0x82F63B78
    for value in data:
        state ^= value
        for _ in range(8):
            state = (state >> 1) ^ (0x82F63B78 if state & 1 else 0)
    return state.to_bytes(4, 'little')


def frame(identifier: int, payload: bytes) -> bytes:
    header = struct.pack('>QI', identifier, len(payload))
    checksum = crc32c(header)
    return header + checksum + payload + bytes(a ^ b for a, b in zip(crc32c(payload), checksum))


def exact(sock: socket.socket, length: int, deadline: float) -> bytes:
    data = bytearray()
    while len(data) < length:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if data:
                raise RuntimeError('Partial wire frame timed out')
            raise IdleTimeout('No response within the bounded observation window')
        sock.settimeout(remaining)
        try:
            value = sock.recv(length - len(data))
        except TimeoutError as error:
            if data:
                raise RuntimeError('Partial wire frame timed out') from error
            raise IdleTimeout('No response within the bounded observation window') from error
        if not value:
            if data:
                raise RuntimeError('Peer closed inside a wire frame')
            raise EOFError('Peer closed the connection')
        data.extend(value)
    return bytes(data)


def receive(sock: socket.socket, timeout: float) -> tuple[int, bytes]:
    deadline = time.monotonic() + timeout
    header = exact(sock, 16, deadline)
    identifier, length = struct.unpack('>QI', header[:12])
    if length > 1024 * 1024:
        raise RuntimeError(f'Unexpected response length {length}')
    if crc32c(header[:12]) != header[12:]:
        raise RuntimeError('Response header CRC mismatch')
    try:
        payload = exact(sock, length, deadline) if length else b''
        trailer = exact(sock, 4, deadline)
    except (IdleTimeout, EOFError) as error:
        raise RuntimeError('Incomplete response after wire header') from error
    if trailer != bytes(a ^ b for a, b in zip(crc32c(payload), header[12:])):
        raise RuntimeError('Response payload CRC mismatch')
    return identifier, payload


def connect(path: Path, timeout: float) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(str(path))
    except Exception:
        sock.close()
        raise
    return sock


def params(sock: socket.socket, identifier: int, timeout: float) -> tuple[int, int]:
    sock.sendall(frame(identifier, struct.pack('>I', 0)))
    returned, payload = receive(sock, timeout)
    if returned != identifier or len(payload) != 12:
        raise RuntimeError('Incorrect PARAMS response')
    return struct.unpack('>IQ', payload)


def blob(path: Path) -> str:
    data = path.read_bytes()
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()


class Daemon:
    def __init__(self, binary: Path, root: Path, block: int, log: Path):
        self.binary, self.root, self.block = binary, root, block
        self.pidfile, self.sock = root/'pid', root/'socket'
        self.log = log.open('wb')
        self.pid: int | None = None
        self.reaped = False
        self.cleanup = None

    def start(self) -> None:
        storage = self.root/'storage'
        storage.mkdir()
        command = [str(self.binary), '-s', str(self.sock), '-d', str(storage),
                   '-b', str(self.block), '-n', '1', '-p', str(self.pidfile)]
        launcher = subprocess.Popen(command, cwd=self.root, stdout=self.log, stderr=self.log)
        try:
            code = launcher.wait(timeout=15)
        except BaseException:
            launcher.kill()
            launcher.wait(timeout=5)
            if self.pidfile.exists():
                self.pid = int(self.pidfile.read_text())
            raise
        if self.pidfile.exists():
            self.pid = int(self.pidfile.read_text())
        if code != 0 or self.pid is None:
            raise RuntimeError(f'lbs did not start: launcher={code}')
        # The unmodified daemonize() forks once and writes its child PID before
        # the parent exits. The Linux subreaper makes that child ours to reap.
        self._verify_owned_process()

    def _verify_owned_process(self) -> None:
        assert self.pid is not None
        proc = Path('/proc')/str(self.pid)
        if (proc/'exe').resolve() != self.binary.resolve():
            raise RuntimeError('Refusing to signal an unexpected process')
        arguments = (proc/'cmdline').read_bytes().split(b'\0')
        if str(self.pidfile).encode() not in arguments:
            raise RuntimeError('Daemon PID does not belong to this temporary case')

    def alive(self) -> bool:
        assert self.pid is not None
        if self.reaped:
            return False
        found, status = os.waitpid(self.pid, os.WNOHANG)
        if found:
            self.reaped = True
            self.cleanup = {'unexpected_exit_status': status, 'reaped': True}
            return False
        return True

    def stop(self) -> None:
        try:
            if self.pid is None or self.reaped:
                return
            if not self.alive():
                return
            self._verify_owned_process()
            os.kill(self.pid, signal.SIGTERM)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                found, status = os.waitpid(self.pid, os.WNOHANG)
                if found:
                    self.reaped = True
                    self.cleanup = {'signal': 'SIGTERM', 'wait_status': status, 'reaped': True}
                    return
                time.sleep(0.01)
            self._verify_owned_process()
            os.kill(self.pid, signal.SIGKILL)
            _, status = os.waitpid(self.pid, 0)
            self.reaped = True
            self.cleanup = {'signal': 'SIGKILL', 'wait_status': status, 'reaped': True}
        finally:
            self.log.close()


@contextmanager
def daemon(binary: Path, root: Path, block: int, log: Path):
    instance = Daemon(binary, root, block, log)
    try:
        instance.start()
        yield instance
    finally:
        instance.stop()


def exercise(instance: Daemon, kind: str, version: str, timeout: float) -> dict:
    record = {'version': version, 'kind': kind, 'block_size': instance.block}
    bug_path = kind in ['wrong-append', 'append-then-params', 'append-then-params2']
    expected_stall = version == 'base' and bug_path
    payload = bytes((i * 17 + 3) % 256 for i in range(instance.block))
    with connect(instance.sock, timeout) as client:
        block, next_block = params(client, 1, timeout)
        if block != instance.block:
            raise AssertionError('Initial real-server PARAMS handshake failed')
        record['initial_next_block'] = next_block
        expected_next = next_block
        if kind == 'normal-params':
            if params(client, 2, timeout) != (block, next_block):
                raise AssertionError('Second ordinary PARAMS response differs')
            record['connection_end'] = 'normal client close'
        elif kind == 'normal-append':
            client.sendall(frame(2, struct.pack('>IIQ', 2, 1, next_block) + payload))
            identifier, reply = receive(client, timeout)
            expected_next += 1
            if identifier != 2 or reply != struct.pack('>IQ', 0, expected_next):
                raise AssertionError('Ordinary APPEND did not succeed')
            client.sendall(frame(3, struct.pack('>IQ', 1, next_block)))
            identifier, reply = receive(client, timeout)
            if identifier != 3 or reply != struct.pack('>I', 0) + payload:
                raise AssertionError('GET did not return the written block byte-for-byte')
            record['roundtrip_bytes'] = len(payload)
            record['connection_end'] = 'normal client close'
        else:
            if kind == 'wrong-append':
                request = frame(2, struct.pack('>IIQ', 2, 1, next_block) + payload[:-1])
            elif kind == 'unknown-type':
                request = frame(2, struct.pack('>I', 0x7FFFFFFF))
            elif kind == 'zero-block-append':
                request = frame(2, struct.pack('>IIQ', 2, 0, next_block))
            elif kind == 'bad-header-crc':
                request = bytearray(frame(2, struct.pack('>I', 0)))
                request[12] ^= 1
                request = bytes(request)
            elif kind in ['append-then-params', 'append-then-params2']:
                # Both complete frames are below 1 KiB at the selected 512-byte
                # block size. No sleeps or source hooks force writer timing.
                request = frame(2, struct.pack('>IIQ', 2, 1, next_block) + payload)
                request += frame(3, struct.pack('>I', 0 if kind.endswith('-params') else 4))
                expected_next += 1
            else:
                raise ValueError(kind)
            record['request_bytes'] = len(request)
            record['request_sha256'] = hashlib.sha256(request).hexdigest()
            client.sendall(request)
            responses = []
            while True:
                try:
                    identifier, reply = receive(client, timeout)
                except EOFError:
                    ending = 'peer closed'
                    break
                except IdleTimeout:
                    ending = 'timeout'
                    break
                responses.append({'id': identifier, 'payload_hex': reply.hex()})
                if len(responses) > 2:
                    raise AssertionError('Unexpected response sequence')
            expected_responses = []
            if kind.startswith('append-then-'):
                expected_responses = [{'id': 2, 'payload_hex': struct.pack('>IQ', 0, expected_next).hex()}]
            if responses != expected_responses:
                raise AssertionError(f'{kind}: wrong response sequence (writer-busy path may not have triggered): {responses}')
            if ending != ('timeout' if expected_stall else 'peer closed'):
                raise AssertionError(f'{kind}: unexpected connection disposition {ending}')
            record['responses'] = responses
            record['connection_end'] = ending
    if not instance.alive():
        raise AssertionError('Daemon exited instead of remaining available')
    with connect(instance.sock, timeout) as later:
        start = time.monotonic()
        try:
            actual = params(later, 100, timeout)
        except IdleTimeout:
            record['next_client'] = 'timeout'
            if not expected_stall:
                raise AssertionError('A subsequent real client stalled')
        else:
            record['next_client'] = 'answered'
            record['next_client_params'] = list(actual)
            if expected_stall or actual != (block, expected_next):
                raise AssertionError(f'Unexpected subsequent-client PARAMS: {actual}')
        record['next_client_observation_seconds'] = round(time.monotonic() - start, 4)
    if not instance.alive():
        raise AssertionError('Daemon died while observing the subsequent client')
    record['daemon_alive_after_probe'] = True
    record['expected_outcome'] = True
    return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--head', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=2.0)
    args = parser.parse_args()
    if not 0.5 <= args.timeout <= 10:
        parser.error('--timeout must be between 0.5 and 10 seconds')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    if crc32c(b'hello world').hex() != 'ca130baa':
        raise AssertionError('CRC does not match upstream own known-answer vector')
    libc = ctypes.CDLL(None, use_errno=True)
    # PR_SET_CHILD_SUBREAPER: adopt daemonize() children so cleanup is provable.
    if libc.prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), 'Linux child subreaper setup failed')
    report = {'base': BASE, 'head': HEAD, 'timeout_seconds': args.timeout,
              'scope': 'Real unmodified lbs daemons; AF_UNIX only; no instrumentation',
              'crc_vector': 'hello world -> ca130baa', 'sources': {}, 'cases': []}
    kinds = ['normal-params', 'normal-append', 'unknown-type', 'zero-block-append',
             'bad-header-crc', 'wrong-append']
    with tempfile.TemporaryDirectory(prefix='rv331-') as temporary:
        root = Path(temporary)
        for version, source, revision in [('base', args.base.resolve(), BASE), ('head', args.head.resolve(), HEAD)]:
            actual_revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
            if actual_revision != revision:
                raise RuntimeError(f'{version}: unexpected commit {actual_revision}')
            manifest = dict(COMMON, **{'lbs/dispatch.c': DISPATCH[version]})
            for relative, wanted in manifest.items():
                if blob(source/relative) != wanted:
                    raise RuntimeError(f'{version}: source mismatch for {relative}')
            subprocess.run(['git', 'diff', '--exit-code'], cwd=source, check=True, capture_output=True)
            binary = source/'lbs/lbs'
            if not os.access(binary, os.X_OK):
                raise RuntimeError(f'Build the real {binary} first')
            report['sources'][version] = {'commit': revision, 'blobs': manifest,
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
            for block in [512, 4096]:
                selected = kinds + (['append-then-params', 'append-then-params2'] if block == 512 else [])
                for kind in selected:
                    name = f'{version}-{block}-{kind}'
                    case_root = root/name
                    case_root.mkdir()
                    record = None
                    with daemon(binary, case_root, block, output/f'{name}.daemon.log') as instance:
                        record = exercise(instance, kind, version, args.timeout)
                    record['cleanup'] = instance.cleanup
                    if not instance.reaped:
                        raise AssertionError('Test daemon was not reaped')
                    report['cases'].append(record)
                    (output/f'{name}.json').write_text(json.dumps(record, indent=2)+'\n')
                    print(f"PASS {name}: {record['connection_end']}; next client {record['next_client']}; daemon reaped", flush=True)
            subprocess.run(['git', 'diff', '--exit-code'], cwd=source, check=True, capture_output=True)
    report['passed'] = len(report['cases'])
    if report['passed'] != 28:
        raise AssertionError('Incomplete matrix')
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('RESULT 28/28 expected real-daemon scenarios; all three drop paths distinguish base/head; normal and pre-increment rejection controls reconnect; all daemons reaped.', flush=True)

if __name__ == '__main__':
    main()
