# LATTICE: kivaloo PR 333 cleanup validation

Independent test-only support for [Tarsnap/kivaloo PR 333](https://github.com/Tarsnap/kivaloo/pull/333). Original report, implementation, and sponsor ownership remain with D. No original implementation branch was modified and no duplicate upstream report was submitted.

## Result: validated with an explicit portability limitation

**82 of 82 expected outcomes matched.** This includes intentionally bad baseline behavior and an explicitly recorded retained-descriptor behavior on the submitted head. It is not a claim that 82 scenarios prove the patch universally correct.

[Corrected run 34069432347](https://github.com/woahwhattheheck/kivaloo/actions/runs/34069432347), job `101583915394`, executed September 7, 2026 UTC (September 6 US Central/Eastern). Tested review commit: `6b3070bf597d911df72d540caa97e87ef6c0abbb`. This report was added afterward without changing the replay or production code.

| Model / scope | Base | Submitted head |
|---|---|---|
| Two `lbs` conduit close sites, each with EIO, EBADF, or EINPROGRESS failure models | All six cases repeatedly close the descriptor until the probe's ninth-call guard. State and idle-array cleanup are not reached. | One attempt per descriptor; both conduit ends attempted; status -1; state and both arrays freed. |
| Five close sites, EINTR after descriptor release and reuse | All five close the unrelated replacement descriptor on retry. | All five preserve the replacement descriptor, with one close attempt. |
| Five close sites, EINTR while original descriptor remains open | Retry closes the original descriptor; status 0. | **Status 0 with the original descriptor still open. This is the portability limitation, not a successful cleanup outcome.** |
| Directory close reports EINTR after releasing the stream | Attempts to pass the released `DIR *` to `closedir` again. The probe stops before a real double-free. | One close attempt and non-null result queue. |
| Directory success and EIO controls | One close; success returns a queue, EIO returns NULL with a warning. | Same expected behavior. |
| Worker-error controls | All three worker-kill calls, cancellation, and both conduit close attempts occur; state/arrays freed; status -1. | Same expected behavior. |
| Ordinary success and stale errno controls | Expected status and cleanup. | Expected status and cleanup. |

Count: two three-component build records, 70 close-model invocations (five sites × seven models × two revisions), four worker-error controls, and six directory invocations = 82 checked outcomes. All eight probe compilations had empty stderr. The recorded probe outputs contain no AddressSanitizer or UndefinedBehaviorSanitizer diagnostics.

## Platform-policy boundary

The new source comments' unqualified statement that POSIX leaves descriptor state unspecified on EINTR describes older POSIX versions. The Linux man-pages project explicitly documents that POSIX.1-2024 instead selected EINTR-leaves-open behavior, while Linux uses the opposite descriptor-lifetime behavior. See [Linux close(2), portability discussion](https://man7.org/linux/man-pages/man2/close.2.html) and the [Austin Group issue history](https://www.austingroupbugs.net/view.php?id=529).

The probe deliberately models both lifetime choices. Its retained-fd observation is a consequence of the submitted code under that model, not evidence that Linux itself returns EINTR with an open descriptor. Conversely, the release-and-reuse probe performs actual `close`/`dup2` operations in a controlled sequence; it does not claim to have reproduced a naturally occurring multi-threaded race.

Do not restore unconditional retries on Linux: the replacement-descriptor case demonstrates the hazard. Scope the rationale to the intended platforms and standard vintage, and make support for platforms retaining the descriptor an explicit implementation decision. The `closedir` experiment is separately scoped to a stream already released by the injected wrapper; it is not a claim that every libc has the same internal behavior.

## Provenance

| Item | Value |
|---|---|
| Base | `3de151b5d878b0714552c3658562eb5a87b378ce` |
| Submitted head | `9b517f6df327737183328b9d307b21ee4b613988` |
| Tested review commit | `6b3070bf597d911df72d540caa97e87ef6c0abbb` |
| Review branch | `review/lattice-333-cleanup-validation-20260906` |
| Artifact | `9999977972` |
| ZIP SHA256 | `1bdb4ecdfd779fb8e225ff8de2f1c58649f3c85f94ed955457386df096412115` |

[Raw evidence](https://github.com/woahwhattheheck/kivaloo/actions/runs/34069432347/artifacts/9999977972): 53,797 bytes, 196 files, independently downloaded and hash-verified. It contains the exact probes, original source blob manifest, before/after source hashes, environment, compile/build output, every invocation's stdout/stderr, expected/observed JSON, and summary. GitHub artifact retention expires September 21, 2026 UTC; the replay and this document remain in Git.

| Source | Base Git blob | Head Git blob |
|---|---|---|
| `lbs/dispatch.c` | `55f495518684a3a6c85993eb5ac93c88152f42ba` | `d8eaa1e5aad5df30308e63eef7ca10aeecf37a1a` |
| `lbs-s3/dispatch.c` | `b30dc4f80dc544643550e9106437a7c4939c00e2` | `7732d1191ab16b6cc783e2d8460051dc6f61c3ae` |
| `lbs-dynamodb/dispatch.c` | `cb9c6be44802bb9592c7f6961f2515f8f2e15a9a` | `f6fa9972db33160b052ee075e07b8cc441c22bef` |
| `lbs/storage_findfiles.c` | `cb11d2290498f85c6720bc2a90a1aa3f31e482e1` | `ace0cd9e755a28e94fab22b661a7284afa2aa659` |

## Replay and isolation

Use a Linux Git checkout containing both pinned revisions and this review branch, with GCC, make, OpenSSL development libraries, and Python supporting tar extraction filters:

```sh
python3 tests/lattice-333/run.py /absolute/path/to/new-results-directory
```

The output directory must not exist. The runner archives exact Git revisions into temporary directories and runs:

```sh
make -j2 'PROGS=lbs lbs-s3 lbs-dynamodb' 'TESTS='
```

It includes the entire unchanged target translation unit in each probe, with address/undefined-behavior sanitizers and linker garbage collection. External worker and netbuf cleanup functions are isolated instrumentation stubs. Descriptor operations and the storage directory/heap/queue code use real temporary resources. No S3/DynamoDB credentials, live endpoints, production data, or user filesystem fixtures are used.

Repetition is stopped at a ninth close attempt; a second call on a released directory stream is stopped before dereference. Probe guard status 90 is not an application exit status. LeakSanitizer is intentionally disabled because these models include known retained baseline state; this run is not a leak-free certification. In particular, S3/DynamoDB hard-error paths retain dispatcher state in both revisions.

## Failed first attempt, preserved

[Run 34069176107](https://github.com/woahwhattheheck/kivaloo/actions/runs/34069176107) built all three base components but failed at the first probe link before any fault cases executed. My original harness attempted to wrap `warnp`, a macro that expands to `libcperciva_warn`/`libcperciva_warnx`. The correction changed only the three review files to intercept those actual symbols.

[Failed-attempt artifact 9999896685](https://github.com/woahwhattheheck/kivaloo/actions/runs/34069176107/artifacts/9999896685) preserves the linker error and base build. Its upload-reported ZIP SHA256 is `9bf6f86263df13409d4f9586dd255b1442e198a894ff596bdb516748fe245bbe`. It is not presented as a passing run or as a defect in D's implementation.

## Limits

This is a focused exact-source, controlled-failure validation with three-component native builds. It is not a full daemon/cloud integration test, a complete repository test suite, a real-world signal/reuse race reproduction, cross-platform execution, or a security-impact assessment. No maintainer acceptance, merge, bounty award, or payment is inferred.
