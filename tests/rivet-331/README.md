# RIVET: kivaloo #331 real-daemon validation

Completed September 6, 2026 (America/Chicago). RIVET is a ChatGPT LLM session.
This is an independent review of D's existing submission, not a new upstream
report, bounty claim, or change to the original implementation branch.

## Verified result

Both exact `lbs` applications built normally and all **28 expected protocol
scenario outcomes** passed. No dispatcher, worker, response, or storage function
was mocked or instrumented. Each case used an unmodified daemon, new temporary
storage, and a private Unix-domain socket. The runner checks that the daemon is
still alive before attributing a timeout to the reported liveness defect.

Run: https://github.com/woahwhattheheck/kivaloo/actions/runs/34069411917

Job: `101583861407`. Validation commit:
`820f5935123264f922675ff28e76dd0a66a0101e`.

- Original base: `3de151b5d878b0714552c3658562eb5a87b378ce`
- Submitted fix: `690f3805fc4dc3e97234478f6eb1aed9d3d163e5`
- Original dispatch blob: `55f495518684a3a6c85993eb5ac93c88152f42ba`
- Fixed dispatch blob: `420a06e0a96db7ed7e18fc7c4052201c9c8fb5b3`
- Driver blob: `7b36597d5075d058c92e24773c3e2f31f72984f8`

Ubuntu 24.04.4, GCC 13.3.0, ordinary `-O2` build, GNU Make 4.3,
Python 3.12.3. The job verifies the exact driver hash, commit identities, and
six relevant source blobs per revision. `git diff --exit-code` before and after
execution confirms no tracked source changes. The report records binary hashes.

## Differential and controls

For each revision, six scenarios run at both 512-byte and 4096-byte block sizes:
ordinary PARAMS, ordinary APPEND followed by a byte-exact GET, unknown request
type, zero-block APPEND, bad header CRC, and wrong-size APPEND. Two additional
512-byte scenarios send a valid APPEND immediately followed by PARAMS or PARAMS2.
That is 14 scenarios per revision, 28 total.

The four affected cases per revision exercise all three post-increment drop
sites. On base, the wrong-size APPEND cases and both writer-busy cases leave the
daemon alive but the rejected connection open. A fresh client subsequently
connects but receives no PARAMS response in its two-second observation window.
On head, the rejected connection closes and the next client receives the correct
PARAMS response. Both pipelined writer-busy scenarios first receive the valid
APPEND's successful response, and the next client sees the next block number
advance to 1. This checks real work completion as well as connection recovery.

The pipeline uses two complete frames in one send, under 1 KiB at the selected
block size. No artificial worker delay, source hook, or response stub forces
timing. The driver fails rather than claiming coverage if the expected
writer-busy response/drop sequence is not observed. This run observed that
sequence for both PARAMS variants on both revisions; it is not an exhaustive
scheduler-interleaving proof.

The ordinary controls write/read real temporary storage and reconnect. All
three earlier-rejection controls close and reconnect on both revisions. In
particular, these protect the distinction between the parser failure before
`npending` is raised and the three later rejections which must give it back.

The timeouts are bounded observations, not an empirical proof of an infinite
hang. The source-level ghost-pending-count explanation is D's existing report:
https://github.com/Tarsnap/kivaloo/pull/331

## Process and wire integrity

The Python driver uses Linux child-subreaper support to adopt the original
`daemonize()` child. It verifies the executable and its case-specific pidfile
argument before signalling it. All 28 daemons were stopped with SIGTERM and
reaped with `waitpid`; no SIGKILL fallback was used. No daemon was left running.

Wire response IDs, lengths, header checksums, and payload checksums are checked.
The CRC implementation matches libcperciva's unusual implicit-leading-bit
initialization and its own known-answer vector, `hello world -> ca130baa`.
Incomplete frames, unexpected daemon exits, and wrong response sequences fail
the test rather than being counted as a timeout reproducing the defect.

## Evidence and replay

64-file raw artifact:
https://github.com/woahwhattheheck/kivaloo/actions/runs/34069411917/artifacts/9999968230

Downloaded archive SHA256, independently checked:
`9e0fde2b8d15480cebe11fcfbaa5aa7a510c1a6759ee27db6d4d9cac6c4c9c29`.

The archive contains both build logs, the environment and pinned revision
record, the protocol console, 28 daemon logs, 28 per-case JSON records, and the
complete report. The artifact retention is 14 days; the runner and recipe remain
in the review branch.

From a checkout of the validation commit with both pinned Git objects present:

```sh
git worktree add --detach /tmp/rivet331-base 3de151b5d878b0714552c3658562eb5a87b378ce
git worktree add --detach /tmp/rivet331-head 690f3805fc4dc3e97234478f6eb1aed9d3d163e5
make -C /tmp/rivet331-base/lbs -j2
make -C /tmp/rivet331-head/lbs -j2
python3 tests/rivet-331/run.py --base /tmp/rivet331-base --head /tmp/rivet331-head --output /tmp/rivet331-results --timeout 2
```

Use new worktree/output paths. The runner deliberately refuses an existing
output directory. The workflow shows the public-source fetch and prerequisite
installation. Local checks before upload comprised Python syntax, the CRC
known-answer test, and socketpair frame roundtrip; all real-daemon evidence is
from the hosted run cited above.

## Limits and ownership

This is focused Linux x86-64 protocol validation, not the full project test
suite, sanitizer certification, all block sizes or race interleavings, or
non-Linux portability testing. It establishes neither a bounty tier nor payment
or acceptance. No credentials, live cloud service, public/TCP listener, or
production files were accessed. D's implementation and sponsor/report ownership
remain unchanged. No upstream comment, duplicate submission, or merge was made.
