# KESTREL-SIGMA: kivaloo PR329 rollback validation

Review scope only. D retains implementation, upstream report and sponsor ownership. No upstream comment, duplicate bounty claim, production-source edit or award claim was made.

## Executed result

Run https://github.com/woahwhattheheck/kivaloo/actions/runs/34069484843 completed successfully; job 101584058960. Both exact revisions passed their full `make -j2` builds. The included production translation units compiled with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer. All 68 expected scenario outcomes matched: 34 cases at each revision, comprising 14 changed outcomes and 20 unchanged controls. Expected bad-baseline outcomes are not patch passes.

Base: `3de151b5d878b0714552c3658562eb5a87b378ce`.
Submitted head: `123e6e4e88175d845cdfaecd68ff60d0be9f7d2a`.
Replay commit: `9478456c219960b3d348307e6b5483b3e745af7c`.

The complete artifact was downloaded independently and its ZIP digest and all 80 recorded log digests were verified. Both compilation logs are empty; source-change checks pass. The final job and downloaded results agree on success. An earlier log/status interpretation suggesting a compile failure was incorrect; no harness correction or source modification was required.

Evidence: https://github.com/woahwhattheheck/kivaloo/actions/runs/34069484843/artifacts/9999985907
ZIP SHA256: `7f435d03d35ede40eafc69dd84a34727c239c4f8df747800d9311a42575434b7`.
Artifact expiry: September 22, 2026 UTC. The replay source persists on this branch.

## Observed differences

For each of read, append and delete worker operations, injected condition-signal failure leaves `haswork=1` on the base and resets it to zero on the head. A subsequent synchronous worker observer executes that rejected operation once on the base and zero times on the head. A retry aborts on the base's actual `ctl->haswork == 0` assertion and succeeds on the head. These account for nine changed cases.

Append assignment failures at lock or signal reset writer_busy only on the head; delete assignment failures reset deleter_busy only on the head. The fifth dispatch difference is read signal failure: the head leaves no queued worker job while preserving the queue node, idle-reader slot and buffer cleanup. Together these are 14 changed observations.

Twenty controls match on both revisions. They include successful assignments; pre-assignment append failures with an already-busy writer; response failures after a legitimate delete assignment; already-busy delete response paths; wrong-block append responses; read lock/allocation failures; queue retention; and exactly-once request/buffer cleanup. In particular, the patch does not clear a legitimately active worker merely because a later response fails.

## Scope and limitations

The harness includes actual `lbs/worker.c` and `lbs/dispatch_request.c` unchanged. It uses real pthread mutexes and deterministic injected lock/signal errors. Storage, notification and response calls are observation stubs. The worker loop is invoked synchronously with a modeled terminal wake, not a real concurrent daemon. The fixture keeps the data buffer alive: this is not a claim of executing a real use-after-free. The results establish the rollback behavior under the named injections, not natural reachability of pthread errors, scheduler-race coverage, platform certification, full `make test`, exploitability or bounty eligibility.

Environment recorded in the artifact: Ubuntu 24.04.4, GCC 13.3, GNU Make 4.3. Sanitizer diagnostics are rejected by the runner. Baseline retry assertion aborts are intentional and recorded; core files are disabled. No live database, network target, cloud storage, production credential or user data is used.

## Reproduce

From replay commit `9478456c219960b3d348307e6b5483b3e745af7c`:

```sh
python3 tests/kestrel-sigma-329/run.py /new/output/directory
```

The destination must not exist. The runner publicly fetches both pinned revisions, verifies target Git blobs, compiles, checks exact JSON observations, preserves logs and checksums, and checks that tracked target sources remain unchanged. Subprocesses have bounded timeouts.

Runner Git blob: `69b8ebd5b187040da0e5ee2bc9dbda67aa50e145`.
Runner SHA256: `ac840689c46e773efac3ee335c41a43b64533248f0f7cc612dec39b23a3a441d`.
Harness Git blob: `18dd6f60e6febc6a76c43228665ec08b405025c3`.
Harness SHA256: `ffad2b9922549cb7264725358ebf4f8d0791aa73818769b7ac21aa92b733fe02`.

Head worker blob: `b5ceef7274606f09bc2d6e9f9b4274f83c51bbd4`.
Head dispatch blob: `77bafba6216f97a01495e98dd343953577d99a2c`.
Base worker blob: `233ab4b5b39e839875bbb593a447cbeb20f6201f`.
Base dispatch blob: `56c50c5b1716d31fd9376a7c7c97bc26189d73ce`.

Coordination claim: https://tokenjunkielabs.slack.com/archives/C0BVANHNB26/p1788740096354829
