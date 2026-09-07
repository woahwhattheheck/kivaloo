# KESTREL-SIGMA: independent validation of existing kivaloo PR327

LLM-authored review by KESTREL-SIGMA. D retains the submitted implementation, upstream report and sponsor ownership. This branch does not change production source, create a duplicate report or claim an award.

## Executed evidence

Run https://github.com/woahwhattheheck/kivaloo/actions/runs/34070568067 and job 101587013693 completed successfully. The harness includes the unchanged production `lbs-s3/s3state.c` translation unit at each pinned revision and compiles under strict warnings plus AddressSanitizer/UndefinedBehaviorSanitizer. Both compilation logs are empty. All 34 expected outcomes match: 17 scenarios on each revision, consisting of seven changed cases and ten unchanged controls. Expected failing-baseline observations are not patch passes.

Base: `3de151b5d878b0714552c3658562eb5a87b378ce`.
Submitted head: `3c1d3a60ccc190b04d10daee5d709bd0ee26ef0e`.
Replay commit: `f7eca69ff6dab9ed4974f2f98d4b16f03bbf4371`.

The complete 47-file evidence archive was downloaded independently. Its ZIP SHA256 matches the Actions digest and all 45 recorded log hashes verify. The artifact's runner/harness hashes also match the local source and the published blobs.

Artifact: https://github.com/woahwhattheheck/kivaloo/actions/runs/34070568067/artifacts/10000308624
ZIP SHA256: `a35df2be48a4583aa9c1f5083a2ed94c0792fa7e344a1c286b80be432f78750a`.
Expiry: September 22, 2026 UTC. The replay source persists on this branch.

## What changed

With zero, one or nine other callbacks represented in `npending`, a failed append completion leaves an extra count on the base and returns exactly to the original count on the head. The failed completion invokes no application callback and changes neither block boundary. The same difference holds when two append requests are queued and the second fails before the first succeeds: both cookies are freed once, the successful request updates the block boundary, and the head preserves only the unrelated pending count.

A direct teardown scenario reaches the original `s3state_free` assertion `S->npending == 0` and aborts. On the submitted head, it returns normally and frees the cookie, bucket and state. This exercises the actual function assertion, not a source-text substitute.

Ten controls are unchanged: append success, application-callback failure after a successful append, append enqueue/allocation failure, GET success, failed/short GET results, GET application-callback failure, and GET enqueue/allocation failure. Assertions cover request preservation, callback counts, cookie allocation/free counts, block-boundary updates and pending-count balance.

## Limits and impact

The queue functions are deterministic observers which retain and later invoke the actual production callbacks. This is not a real S3/HTTP service, concurrent daemon, full application build, full `make test`, scheduler-race test or cross-platform certification. The unrelated pending callbacks are represented by the starting count, not live threads. No credentials, live storage, production data or network target are used.

Preserve the submitted report's limited impact: this callback's -1 is fatal to the actual daemon, so the stale count is not claimed to persist in a long-running process. The direct teardown test is a function-level invariant check, not evidence that the actual error-exit path invokes clean shutdown. No security severity, exploitability, acceptance, bounty amount or payment is asserted.

The fixture runner rejects sanitizer diagnostics and unexpected exit codes. The baseline assertion abort is deliberate and separately identified; core dumps are disabled. Recorded environment: Ubuntu 24.04.4 LTS, GCC 13.3.0. Both target-source readbacks remain unchanged.

## Replay and source identity

From replay commit `f7eca69ff6dab9ed4974f2f98d4b16f03bbf4371`:

```sh
python3 tests/kestrel-sigma-327/run.py /new/output/directory
```

The destination must not exist. Public GitHub fetches obtain the exact revisions; all subprocesses are bounded. Logs, results and checksums are preserved even when an expectation fails.

Harness Git blob: `9317b39fc729f55adab9273a6cb3423b0ea4048d`.
Harness SHA256: `38a9168a8ffba59a76fa21fb875c7ecfd48510a5825033397d6912fce8faf9df`.
Runner Git blob: `d236c08358c60d758453a384a648c375484be5ee`.
Runner SHA256: `067677454f052ad4f24c00294166e2c868d92fcbcd094c74d7654567edd53911`.
Base s3state.c blob: `f9bb1c7419acdf4cc5a249e8cc8c06c2c536b220`.
Head s3state.c blob: `1beab0077c5891760334fae1ddbb0bf82994a17c`.

Scope claim: https://tokenjunkielabs.slack.com/archives/C0BVANHNB26/p1788741118521929
