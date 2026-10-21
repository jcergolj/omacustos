# Incremental validation and rollout gate (#62)

> Historical preparatory report, preserved as evidence. Current integration and
> validation status are tracked in [Default incremental backups](incremental-default-rollout.md).

Status: **not ready for default rollout**. This validation work does not close #62.
The normal worker still creates full v3 archive copies. No opt-in release, default
switch, compression change, cloud access, or deployment is part of this change.

## Reproducible local benchmark

Build the real core implementation with Release optimizations:

```sh
cmake -S native -B /tmp/opencode/omacustos-62-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/opencode/omacustos-62-release --target incremental-benchmark -j 4
```

From a writable **disk-backed** working directory, run:

```sh
/tmp/opencode/omacustos-62-release/incremental-benchmark 1073741824 4096 64000000 documents-code
/tmp/opencode/omacustos-62-release/incremental-benchmark 1073741824 4096 256000000 documents-code
/tmp/opencode/omacustos-62-release/incremental-benchmark 1073741824 4096 1000000000 documents-code
```

Repeat with `mixed` for deterministic half-code/half-pseudorandom file content.
Arguments are selected bytes, file count, uncompressed archive target, and profile.
Fixtures are synthetic, not a representative customer corpus. Seeds and source
generation are fixed; identities/timestamps vary. Each invocation creates and
removes its own private local fixture, and requires eight times the selected
bytes free as a conservative fixture capacity check. It never uses Proton or a
configured backup root. Compression stays tar.gz with the production defaults.
The fixture uses a staging budget of at least four times the archive target and
raises the file-count limit to the fixture file count, so those constraints do not
silently cap the sizing comparison. This is **not** a measurement of the normal
1 GB staging budget or 1,000-file group limit.

JSON lines record these scenarios separately:

- Fresh v4 baseline; unchanged check; small edit plus rename and deletion.
- Full restore of both old and new states after hiding local coordination state.
- Single-file restore of an **unchanged** file from its original archive, rather
  than the small new archive containing the edited file. Select the middle
  logical path to avoid biasing the result toward a short final archive.
- Exact-proposal preview/confirmation and cleanup down to one saved state.
- Full restore of the retained state after cleanup.

Independent SHA-256 fixtures and exact output file counts check every restore.
An unchanged check must publish no manifest and upload no objects. Renames and
deletions must not retransmit unchanged payloads: only the edited file is packed.
Cleanup must require confirmation, perform no deletion during preview, and leave
exactly the intended newest snapshot.
Two tiny CTest smoke cases cover both fixture profiles; timings are never gates.

## Measurement boundaries

- Total scenario time excludes fixture generation, independent expected hashes,
  and post-restore comparison. Restore time includes discovery/verification and
  placement. Cleanup includes proposal, confirmed execution, and readback discovery.
- `boundary_intervals_ns` sums time since the preceding durable checkpoint into
  the named **ending** boundary. `planned`/`capturing` includes scan/hash work.
  Preparation/capture and compression span `preparing`, `prepared-sources`, and
  `prepared-local`: packing also emits control polls, so these cannot isolate
  compression CPU time. `uploaded` and
  `object-verified` include transfer/verification and ordering overhead. Repeated
  boundary names accumulate. These are not exclusive CPU subphase measurements.
- Provider operation counts and nanoseconds measure individual local calls;
  they overlap boundary/wall times and must not be added to them. Uploaded and
  downloaded bytes count successful local transfers, including control metadata.
  They do not measure failed partial transfers or network wire overhead.
- The provider withholds checksums to exercise download/hash verification, but
  delegates metadata inspection to `LocalProvider`, which hashes internally.
  Consequently even unchanged runs include extra local reads. No CLI startup,
  encryption, network latency, rate limiting, or real quota behavior is measured.
- `checkpoint_sampled_writer_staging_peak_bytes` is a lower-bound sample at
  checkpoints and provider boundaries, not an exact high-water mark. It excludes
  restore staging, source/restore trees, and independent member extraction space.
  Frequent `capturing`/`preparing` control polls do not walk staging; doing so would
  add quadratic measurement overhead as the file group grows.
- Inventory records distinguish compressed referenced archive bytes from total
  physical/obsolete uncompressed member bytes in the newest snapshot. Obsolete
  member bytes are **not** reclaimable compressed bytes, nor necessarily obsolete
  across retained history. Shared archives are not compacted.
- These probes do not compare to fresh v3 full backups, isolate scan/hash from
  journal overhead, or provide authorized-cloud or large-corpus evidence.

## Remaining release blockers

1. Integrate stable local computer identity and repository ownership with normal
   backup-set creation, worker scheduling, queue pause/cancel/resume, and recovery.
   Imported/restored configurations must remain read-only until explicit takeover;
   never silently initialize a new writer over existing history.
2. Scan the configured roots/inclusions/exclusions into complete source mappings;
   preserve scan failure as a failure, not a deletion. Preserve the durable format
   of interrupted legacy copies rather than converting them during resume.
3. Surface first-baseline capacity estimates/warnings (old history plus new
   baseline), distinct successful-check/saved-change/integrity status, cleanup
   attention, and mixed-format history in normal worker/UI paths.
4. Integrate newest-N retention with exact deletion authorization. The advanced
   executor requires explicit per-proposal confirmation; do not bypass that safety
   protocol just to make scheduled cleanup automatic.
5. Add worker/GUI end-to-end failure-injection tests for these integration paths.
   Existing advanced-executor tests alone do not prove the default upgrade path.
6. Gather representative sizing evidence, including v3 comparison, repeats,
   larger corpora where practical, and accurate staging/subphase instrumentation.
   Select the production target only from sufficient evidence. The current
   approximately 1 GB target remains unchanged; that is not a measured selection
   or a performance promise.

Whole-file edits require whole-file retransmission. Capture is non-atomic,
file-by-file, not an application-consistent filesystem snapshot. Lightweight
existence/size reuse checks may miss same-size corruption until a scheduled full
integrity check or restore. A partially live archive retains obsolete members.
No benchmark authorizes weakening verification, retained-history protection,
publication ordering, exact-scope deletion, or offline ownership handoff.

## Existing failure/recovery coverage

These are advanced-executor/component tests, **not** proof of default-worker
rollout. Relevant cases in `native/tests/incrementalrepository_test.cpp` include:

| Behavior | Test cases |
| --- | --- |
| Publication/preparation/upload interruption | `interruptionRecovery`, `lostResponseRecovery`, `publicationIntentDoesNotOverrideCancel` |
| Pause/cancel and protected references | `identityFailureCanResumeOrCancel`, `singleOwnerAndProtectedWork`, `publishedSnapshotWinsLateCancel`, `cleanupProtectsFreshReferencesAndRejectsUnsafeScopes` |
| Mutation during capture | `sourceChangesAndPublicationFreeze`, `changedSincePlanningIsNotUploaded` |
| Quota/corrupt uploads | `quotaAndCorruptUploadsCannotPublish`, `loweredBudgetBlocksMultiMemberRecoveryDownload` |
| Remote loss/damage/uncertainty | `repairedObjectsDoNotRepairHistory`, `networkUncertaintyDoesNotBecomeAbsence`, `sizeOnlyDailyChecksExposeDelayedDetection` |
| Unreadable manifests and cleanup interruptions | `cleanupFailsClosedAndResumesWriter`, `cleanupInterruptions`, `cleanupProposalPublicationFailureIsRecoverable` |
| Shared reachability and mixed-format retention | `cleanupPreservesSharedMembersAndSuccessfulHistory`, `cleanupEmptyMixedStatesAndQuotaFailure` |
| Scheduled full integrity and interrupted coverage | `fullIntegrityDetectsHistoricalSameSizeDamage`, `integrityBudgetResumeAndCoverage`, `integrityLostCheckpointResponse`, `integrityReconcilesNewReachabilityAndProtectedUploads` |

`native/tests/incrementalrestore_test.cpp` separately covers remote-only recovery,
old/new shared snapshots, malformed archives, selection, and offline takeover.
Legacy restore/continuation coverage remains in the existing backup engine,
retention/discovery, and worker recovery suites.

## Execution environment (2026-10-20)

Core base: `ae7a9207df1956896a1dd6ad49cbef375780d94d`; only the benchmark and CMake
test registration are added to that implementation. Host: Intel i5-9600K,
6 cores, GCC 16.2.1, Qt 6.12.0, libarchive 3.8.10, zlib 1.3.2, Release `-O3`.
Fixtures run on the local Btrfs SSD mount with filesystem compression enabled.
Reported file/tree bytes are logical file sizes, not allocated disk space. These
are exploratory runs, not repeated performance measurements or default-selection
evidence. Earlier runs with per-read staging-tree sampling were discarded: that
probe disproportionately slowed larger file groups and was corrected before
recording final measurements.

Approximately 100 GiB was available. The conservative eight-times fixture guard
requires 160 GB for a 20 GB selected corpus; therefore 20/50/100/500 GB runs were
not practical on this filesystem and were not attempted. No cloud measurements
were authorized or performed.

Pre-commit independent reviews covered standards and #62 separately. Both found
the archive-upload-count assertion insufficient; it now checks the sole newly
uploaded archive member's size and SHA-256. The standards review additionally
requested exact post-cleanup snapshot enumeration, now asserted before retained
restore. Both re-reviews found no remaining blockers in this **preparatory**
scope, while the spec review explicitly retained the default-rollout blockers.

## Automated test results

- `cmake --build build -j 4` succeeded.
- `ctest --test-dir build --output-on-failure -j 2`: **29/30 suites passed**;
  `workerrecovery-test` reached its existing 180-second timeout during concurrent
  benchmarking, with no assertion failure reported before timeout. This was not
  a clean full-suite run.
- After stopping benchmark load,
  `ctest --test-dir build -R '^workerrecovery-test$' --output-on-failure`:
  **1/1 passed in 176.33 seconds**. The narrow timeout margin remains a test-budget
  concern; no timeout, integrity, or recovery behavior was changed to get a pass.
- Final Release rebuild and
  `ctest --test-dir /tmp/opencode/omacustos-62-release -R incremental-benchmark --output-on-failure`:
  **2/2 passed**, also independently rerun by both reviewers.

All 30 suites passed across those invocations, not together in one clean run.
These results do not remove the rollout blockers above.

## Exploratory sizing results

Final corrected-probe runs: one invocation per profile/target, **six passed**.
Each selected 1,073,741,824 bytes in 4,096 files. Final benchmarks ran after the
isolated worker-recovery test finished, without concurrent test-suite load.
`D` = synthetic documents/code; `M` = half code/half pseudorandom bytes. Targets
are decimal MB; durations below are seconds. These are v4-only results, not
measured speedups over v3 or performance promises.

| Profile | Target MB | First | Unchanged | Edit | Old full restore | New full restore | Single restore | Cleanup | Retained restore |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| D | 64 | 20.464 | 3.771 | 4.146 | 22.342 | 22.451 | 0.601 | 6.759 | 23.357 |
| D | 256 | 20.139 | 3.696 | 4.197 | 23.852 | 22.504 | 1.325 | 6.549 | 24.048 |
| D | 1000 | 20.147 | 3.728 | 4.224 | 21.474 | 22.122 | 4.114 | 6.683 | 21.234 |
| M | 64 | 35.131 | 4.703 | 6.158 | 26.952 | 24.867 | 1.660 | 11.738 | 27.417 |
| M | 256 | 33.987 | 4.683 | 6.167 | 24.197 | 25.249 | 2.557 | 11.574 | 26.606 |
| M | 1000 | 33.491 | 4.695 | 6.165 | 22.481 | 24.642 | 6.117 | 11.560 | 22.489 |

Transfer bytes include control metadata, except the explicitly named compressed
payload column. `U/D` means uploaded/downloaded bytes. Every unchanged run uploaded
**zero payload objects**, but still transferred control metadata. Each edit
uploaded one 15,911-byte compressed object containing the entire edited
262,159-byte file, rather than only the appended 15 bytes.

| Profile | Target MB | Baseline objects | Compressed payload bytes | First U/D | Unchanged U/D | Edit U/D | Single-file download bytes |
| --- | ---: | ---: | ---: | --- | --- | --- | ---: |
| D | 64 | 17 | 62,978,696 | 66,687,923 / 66,704,105 | 455,872 / 3,259,036 | 3,272,873 / 6,078,666 | 5,145,595 |
| D | 256 | 5 | 63,002,715 | 66,703,030 / 66,709,816 | 452,612 / 3,251,896 | 3,265,733 / 6,067,646 | 16,385,971 |
| D | 1000 | 2 | 62,809,338 | 66,524,293 / 66,528,730 | 454,607 / 3,264,169 | 3,278,006 / 6,090,197 | 59,884,122 |
| M | 64 | 17 | 572,749,896 | 576,459,191 / 576,475,373 | 455,889 / 3,259,087 | 3,272,924 / 6,078,751 | 35,512,345 |
| M | 256 | 5 | 572,772,582 | 576,472,917 / 576,479,703 | 452,617 / 3,251,911 | 3,265,748 / 6,067,671 | 137,848,253 |
| M | 1000 | 2 | 572,756,231 | 576,471,194 / 576,475,631 | 454,609 / 3,264,175 | 3,278,012 / 6,090,207 | 534,735,972 |

Provider call totals are identical between profiles at each target:

| Target MB | First | Unchanged | Edit | Old restore | New restore | Single restore | Cleanup | Retained restore |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 362 | 85 | 155 | 36 | 38 | 21 | 390 | 38 |
| 256 | 146 | 61 | 107 | 12 | 14 | 9 | 234 | 14 |
| 1000 | 92 | 55 | 95 | 6 | 8 | 6 | 195 | 8 |

First-run sampled writer staging peaks (bytes), respectively D/M:
64 MB: **71,570,962 / 132,289,822**;
256 MB: **286,037,538 / 529,003,024**;
1000 MB: **1,116,778,426 / 2,066,482,122**.
These are not true peaks and do not represent the normal 1 GB staging budget.

First-run phase evidence below uses the ending-boundary definitions above.
Preparation combines the three preparation boundaries. Verification combines
`object-verified` and `verified-remote`; publication combines prepared/published.
Local provider upload/download times overlap these phases and are not network
throughput measurements. Other lifecycle/journal phases account for the remainder.

| Profile | Target MB | Scan/hash | Preparation/capture/compression | Verify/receipt | Publication | Provider upload | Provider download |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| D | 64 | 2.436 | 12.579 | 4.309 | 0.433 | 0.129 | 0.147 |
| D | 256 | 2.437 | 12.557 | 4.136 | 0.447 | 0.102 | 0.050 |
| D | 1000 | 2.448 | 12.678 | 4.118 | 0.442 | 0.028 | 0.031 |
| M | 64 | 2.442 | 23.366 | 6.300 | 1.410 | 0.967 | 0.214 |
| M | 256 | 2.445 | 23.472 | 6.007 | 1.408 | 0.200 | 0.067 |
| M | 1000 | 2.438 | 23.118 | 6.008 | 1.436 | 0.060 | 0.046 |

In every newest snapshot, referenced archives contain 1,074,003,983 physical
member bytes, including **524,288 obsolete member bytes** (the edited file's old
content plus the deleted file). They remain in partially live archives even
after newest-one retention; the renamed file is reused. This small fixture does
not establish long-term obsolete-data behavior or reclaimable compressed space.

The tradeoff is visible: larger groups reduce calls, while selective restore of
one unchanged 262,144-byte file downloads substantially more data. Compressed
baseline sizes barely vary on these fixtures. These single synthetic runs cannot
choose a production target: keep the current target unchanged pending the
representative/repeated/cloud and exact-staging evidence described above.
