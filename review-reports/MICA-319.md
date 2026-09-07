# MICA — independent validation of Tarsnap/kivaloo PR #319

Completed review-only validation by MICA, a ChatGPT/LLM session acting for the account owner. **Linux release-and-reuse behavior is repaired; retained-descriptor EINTR behavior is not.** A successful expected-outcome replay is not a blanket portability approval.

D retains the original implementation, report and sponsor relationship. This is distinct from LATTICE's #333 worker/shutdown/closedir validation; its portability caveat is preserved rather than re-reported as a new bounty.

## Exact source and execution

- Base: `3de151b5d878b0714552c3658562eb5a87b378ce`.
- Submitted head: `44b96da661657acce648dfa22dd396b307c516f0`, branch `disk-no-close-eintr-retry`.
- Tested review/harness commit: `6c243486a47f45ef3face7e7744333fa976fff2a`.
- `lbs/disk.c` blobs: base `94d6363d8e8ea6ef6e76b37f8a3f7bdc0c2e12a5`, head `8a4436302608b860648939943485795b23ba0d78`.

Both full source trees built using `make -j2 CC=gcc CFLAGS='-O2 -g'`. The observer links each tree's **actual compiled `lbs/disk.o`** and actual supporting library; it does not substitute a modeled disk function. Both observers compiled under `-std=c99 -O2 -g -Wall -Wextra -Werror` with empty compiler stderr. Tracked-source diff checks passed before and after execution.

Environment: Linux x86-64 / Ubuntu 24.04, GCC 13.3.0, GNU Make 4.3, OpenSSL 3.0.13.

Successful run: https://github.com/woahwhattheheck/kivaloo/actions/runs/34071130035

Job `101588562891`, completed/success.

## Results

**250/250 expected outcomes matched; no failed expectations.** There are 125 cases per revision, covering `disk_syncdir`, `disk_read`, zero-length read, create/append writes and their nosync variants.

### Success-path close contracts

Each row below covers seven API variants with otherwise successful I/O. All descriptor identities are checked with real `fstat`; preserved replacement descriptors are also read and written successfully.

| Injected first-close behavior | Original base | Submitted head |
| --- | --- | --- |
| Release original fd, second thread reuses its number, return EINTR | Retries; closes unrelated replacement; returns success | Does not retry; replacement remains readable/writable; returns success |
| Release original fd, no reuse, return EINTR | Retries closed fd; returns failure | Does not retry; returns success |
| Retain original fd, return EINTR | Retries and closes original; returns success | Does not retry; **original remains open despite success** |
| Release fd, return EIO / ENOSPC / EINPROGRESS | One close call; error reported; returns failure | Same one-close/error behavior |
| Ordinary successful close | One close; correct data and return value | Same behavior |

The release-and-reuse seam first calls the real `close`, then starts and joins a real second thread which opens an unrelated temporary sentinel file using the just-released descriptor number. The tested disk code resumes only after that reuse is complete. Thus the old retry's effect on another thread's real descriptor is directly observed without depending on a probabilistic race.

The error returns themselves are deliberately injected. This is **not** a claim that the Linux kernel naturally returned EINTR in this run, nor that an HP-UX or other operating system was executed.

### Existing error-cleanup paths

Ten earlier I/O-failure paths were replayed under each of the three EINTR descriptor contracts. Both revisions already use one-shot close in those cleanup paths. They preserve all ten replacement descriptors under release-and-reuse; under retained-descriptor semantics, both leave ten original descriptors open. That pre-existing portability limitation is not repaired by this patch.

Across the complete matrix, the base preserves ten replacement descriptors and the head preserves seventeen. The base leaves ten retained original descriptors and the head leaves seventeen: the extra seven are the changed success paths under the retained-EINTR contract. Every surviving test-owned descriptor is recorded **before** separate harness cleanup. All 34 test-created reuse threads are joined.

### I/O and diagnostic controls

Byte-exact file and read-buffer checks cover ordinary I/O, seven-byte partial transfers, one-shot interrupted open/read/write/fsync, read EOF, seek/read/write/fsync errors, silent missing-file reads, exclusive-create EEXIST, and nosync behavior. Newly created files have mode 0600. Existing files are not overwritten by failed exclusive-create or permission-error controls.

The released EIO, ENOSPC and EINPROGRESS controls each still produce a close diagnostic and failure return on all seven variants, on both revisions. This verifies that the changed branch does not broadly swallow non-EINTR close errors. It does not turn EINPROGRESS into a universally portable success code.

## Portability interpretation

The Linux man-pages `close(2)` documentation, version 6.18 (2026-02-08), states that Linux releases the descriptor before reporting these close errors and warns against retrying a reused number. It also distinguishes older POSIX's unspecified EINTR state from POSIX.1-2024's retained-descriptor behavior, with which Linux does not conform. Source: https://man7.org/linux/man-pages/man2/close.2.html

Accordingly, the implementation comment's unqualified reference to POSIX leaving the descriptor state unspecified should not be promoted into a current universal guarantee. The tested head deliberately chooses the release-on-EINTR contract; the retained-on-EINTR case remains a limitation requiring the maintainer's supported-platform policy. This report does not invent a new platform-detection fix or edit the original branch.

## Evidence and reproduction

Artifact: https://github.com/woahwhattheheck/kivaloo/actions/runs/34071130035/artifacts/10000493826

Downloaded, ZIP-integrity tested and independently SHA-256 verified: **478 files, 207592 bytes**.

ZIP SHA-256: `19dfdd1eb5c9a9b5f676fe2853a9a32ed5f665e5dbe2f630077b3b35474426c8`.

Observer source SHA-256: `750fe8912b8d57cd6c27cd2285d56699391052463db51c8ba1d6268c0bd199f1`.

The archive contains both observer executables, exact harness sources, full build logs, compile commands, source/object/library/binary hashes, all 250 structured cases, generated temporary-file contents and an empty failed-expectations list. GitHub artifact retention expires September 21, 2026 UTC; the Git recipe and this report persist.

With both pinned revisions normally built under `sources/base` and `sources/head`:

```sh
python3 tests/mica-319/run.py sources results
```

The full bounded, credential-free public-fetch/build sequence is in `.github/workflows/mica-319-validation.yml` at tested review commit `6c243486a47f45ef3face7e7744333fa976fff2a`.

## Scope and ownership

Only synthetic temporary files and injected failure contracts are involved. No production storage, credentials or external service was used. No full native suite, sanitizer, naturally occurring kernel-error frequency, every-interleaving, cross-platform, exploitability or bounty-impact certification is claimed. Original implementation/report/sponsor ownership remains D's, other helpers' lanes remain untouched, and no duplicate upstream issue/PR, acceptance, award or payment is claimed.
