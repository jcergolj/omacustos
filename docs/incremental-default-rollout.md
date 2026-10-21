# Default incremental backups (#62)

Validation status: **local correctness, measurement, Standards, and Spec gates
passed; accepted for commit**. The source now
routes fresh normal-worker runs to v4 incremental storage. No installed service,
cloud account, package, or deployment has been changed by this work.

The earlier [validation report](incremental-validation.md) records preparatory
measurements and the blockers that existed before this integration. It is not the
current implementation status or final delivery evidence.

## Ownership and upgrade

- A locally created configuration is bound to the machine's stable OS identity
  and the fixed absolute configuration directory. Writer UUIDs and epochs are persisted
  before remote initialization; hostnames and editable set names are not writer
  identity. Namespace IDs for historical non-UUID set IDs are deterministic, so
  losing local settings cannot create an unrelated replacement namespace.
- A verified-empty set can be provisioned automatically by a locally bound
  configuration. A nonempty or foreign repository is never silently adopted.
- Imported, copied, and old unbound configurations are read-only. For an old
  pre-ownership configuration, use **Offline writer takeover…** once to enroll its
  verified-empty v4 namespace. This is ownership enrollment, not a format opt-in:
  there is no user-selectable full-upload mode for fresh runs.
- Before confirming enrollment/takeover, stop the previous writer's processes,
  schedules, retries, and access permanently. An in-place upgrade must stop the
  old executable; any subsequent service invocation must use the new writer.
  The protocol is manual-offline, not a server lease or a fencing guarantee.
- Existing remote ownership requires an explicit epoch-changing offline takeover.
  A durable handoff intent makes a lost response retryable. Restoring copied
  local state does not itself restore write authority.
- An unfinished legacy copy resumes in its recorded format and exact namespace,
  after ownership enrollment where needed. Only a fresh run uses the new default.

The first v4 run uploads a fresh baseline and does not reuse legacy payloads.
Before publication work, the worker reports selected uncompressed source bytes
plus the discovered existing-history payload size (or explicitly unknown size).
Compression, manifests, and metadata change actual use; these are estimates, not
quota guarantees. No history is removed to make room for the baseline.

## Saved states, checks, and integrity

All selected regular files are hashed. Intentional skipped links/special nodes do
not invalidate readable regular files. Missing roots, inaccessible directories,
enumeration failures, and unreadable selected files are not treated as deletions.
Execution prunes excluded trees instead of traversing them for preview data.
Durable journals are placed outside selected roots. Volatile application queues,
locks, and private coordination state are excluded and shown excluded in Preview;
the actual settings file remains eligible, so configuration/home selections do
not recursively capture a backup's own changing control state.

New/changed regular files are uploaded whole. Renames and deletions reuse
unchanged content. An unchanged check publishes no snapshot and consumes no
retention slot. Run details distinguish the last successful check from the last
saved change and expose integrity and historical-damage information.

New uploads retain full verification and manifest-last publication. Reused
objects retain original verification evidence and daily existence/size checks;
those lightweight checks can miss same-sized corruption. Scheduled full integrity
checking and verified restores remain necessary. Uploading replacement objects
for a new state does **not** repair older snapshots; affected history is reported.

Capture is file-by-file, not an atomic filesystem/application snapshot. Whole-file
edits retransmit the whole file. Partially live immutable archives retain obsolete
members; compaction and block-level deduplication remain out of scope.

## Cleanup and mixed history

The normal history browser discovers and verifies both legacy copies and shared
snapshots without requiring a local manifest cache. Old non-UUID IDs and legacy
computer/set-name namespaces are associated with the configured set before
retention planning. A shared snapshot cannot be deleted with the old whole-copy
delete action.

Scheduled backups do not authorize their own cleanup. When retention needs work,
the UI reports **cleanup needs attention**. Choose **Review incremental cleanup…**
and review the exact proposed snapshot, legacy-folder, and unreferenced-object
identities. Confirmation authorizes only that proposal. A changed scope requires
a fresh review.

Cancelling a preview writes a dismissal, not deletion authority. A crashed/closed
preview is recovered similarly before the next writer runs. Already-authorized
interrupted cleanup resumes only its durable exact scope. Dismissed proposals
cannot retire snapshots or authorize deletion. Retained and resumable references
still block object deletion, and unreadable history still fails closed.

The newest-N policy counts saved states across old/new formats. Unchanged checks
do not evict history. Saved snapshots survive failed cleanup. Ownership, backup,
integrity maintenance, and cleanup continue to serialize their mutations.

## Validation map

| Boundary | Automated coverage |
| --- | --- |
| Directory open/read/permission failures | `scanerrors-test`, including read failure after a delivered entry, all three historical execution modes, and excluded paths |
| Baseline/unchanged/edit/rename and independent restores | `incrementalworker-test`, existing planner/repository/restore suites |
| Imported/copied/unbound settings, local state loss, enrollment/takeover | `incrementalworker-test` and existing takeover/remote-only restore cases |
| Legacy fresh-baseline upgrade and mixed-format newest-N retention | `mixedFormatUpgradeAndExactRetention`, legacy worker recovery suites |
| Real worker manual/scheduled runs, pause/cancel/resume, lost publication response | `incrementalworker-process-test`, using a local CLI subprocess fixture |
| Real worker quota, partial/corrupt uploads, unreadable retained manifest | `incrementalworker-failures-test` |
| User cleanup preview/cancel/confirm and retained-state restore | `incrementalmaintenance-test`, using the real asynchronous controller and a local CLI fixture |
| Abandoned previews and already-authorized cleanup restart | `incrementalworker-test`, existing cleanup interruption/protected-reference suites |
| Source mutation, missing objects, same-size corruption, scheduled full checks, protected resumable work | Existing `incrementalrepository-test` / `incrementalrestore-test`; the adapter uses the same executor without weakening these gates |

The data-heavy legacy worker suite is registered in two CTest groups instead of
raising its existing 180-second hang timeout. Every legacy test function remains
registered exactly once. New worker process/failure/maintenance suites are also
separate bounded CTest groups.

## Local performance evidence

Build Release and run the repeatable, local-only matrix from disk-backed storage:

```sh
cmake -S native -B /tmp/opencode/omacustos-62-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/opencode/omacustos-62-release --target incremental-benchmark staging-observer -j 4
python3 native/tests/run_incremental_benchmarks.py /tmp/opencode/omacustos-62-release/incremental-benchmark /tmp/opencode/issue62-measurements --bytes 536870912 --files 2048 --observer /tmp/opencode/omacustos-62-release/libstaging-observer.so
```

The matrix separately compares v3 full uploads and v4 first/unchanged/small-edit
runs, full/selective restores, cleanup, and retained-state restore. Profiles are
unmodified current-repository source/QML/docs, an explicitly **amplified** version
of that corpus, and deterministic mixed compressible/pseudorandom data. None is a
customer workload or a cloud throughput measurement. Generated fixture contents
are independently hashed and restored files/counts are checked.

JSON evidence includes provider operation counts/times, transfer bytes, boundary
intervals, dedicated local-plan/hash, capture, compression, publication and
verification measurements, boundary samples, and event-observed private staging
peaks. The observer accounts for regular-file write growth, truncation, removal,
and open unlinked writers; provider-boundary synchronization covers kernel
copies/reflinks. The successful serial fixtures do not release bytes between a
provider copy and that observation. A unit test checks its accounting and a smoke
case requires that it never underreport known boundary samples.
Provider times overlap phase times and must not be added to them. Staging samples
are logical file bytes, not allocated filesystem bytes or a universal process
high-water mark. Restore temporary workspaces are disk-backed; destination trees
and their final atomic-placement temporary files are outside the reported private
workspace scope. Diagnostic observer overhead is included in these local timings.
The matrix uses the production 1 GB staging budget and 1,000-file cap, not the
unconstrained budgets of the earlier sizing probes.

Approximately 106 GB was available before the matrix. The conservative fixture
guard alone needs 160 GB for a 20 GB input; 20/50/100/500 GB fixtures are therefore
not practical here. No cloud measurements were authorized or performed.

## Final local execution evidence — 2026-10-21

`cmake --build build -j 4` succeeded. A clean, single invocation of
`ctest --test-dir build --output-on-failure -j 2` passed **41/41 suites in
427.15 seconds**. Existing timeouts were not increased and no cases were dropped.
An earlier full invocation exposed historical CLI fixtures that still assumed
the old default. Explicitly selecting their historical test seam fixed those
fixtures; all their assertions remain intact. New default-process tests never
use that override.

Release benchmark smoke validation passed **3/3**. The controlled matrix above
then completed **18/18 invocations**, two repeats for each profile/target, with
no concurrent build/test load. Every invocation checked old/new and selective
restored bytes/file counts, unchanged no-payload/no-snapshot behavior, exactly one
edited-file archive, preview-without-deletion, exact cleanup confirmation, and a
byte-verified retained restore. The v4 first-run timing includes initialization;
all v4 runs include source enumeration and full local planning/hashing.

Environment: local disk-backed Btrfs, Intel i5-9600K (6 cores), GCC 16.2.1,
Qt 6.12.0, libarchive 3.8.10, zlib 1.3.2, Release optimizations. All remote
operations used private `LocalProvider` fixtures. Its metadata inspection hashes
files internally even when the exposed checksum is withheld. No CLI startup,
network, encryption, service quota, cloud throughput, or production speedup is
being measured. The small real repository corpus is **slower** with v4's fixed
control/verification overhead; the larger profiles reduce repeated payload work
but have a slower, more thoroughly verified first baseline than v3.

### Inputs and repeat consistency

Both repeats and all three targets used identical independently hashed fixture
contents within each profile. `repository-files` copied the current source/QML
files and README/TECHNICAL documents without amplification. `repository-code`
explicitly repeated those contents; it is not a 512 MiB customer corpus.

| Profile | Selected bytes | Files | SHA-256 of sorted logical names + independent content hashes |
| --- | ---: | ---: | --- |
| Repository files | 859,545 | 96 | `2f36754131929551c52c29ac53e3f010010363addbbb205b7340d31593bf0e64` |
| Amplified repository code | 536,870,912 | 2,048 | `a274c4573b39a3141a7e2f8e7b6f52c8b86356fdd7a34f3d61da9cc9b81e3dad` |
| Synthetic mixed | 536,870,912 | 2,048 | `9743bd810e6e4a4e9631c7d1160b13c34417663176acf058598d1aaabd42d9c8` |

The 18 raw JSONL files contain 234 scenario records plus 18 input records
(158,455 bytes), retained locally at `/tmp/opencode/issue62-final-20261021`.
Receipt SHA-256 over sorted filename, NUL, then file contents:
`8e14a0fe161d0919f99280aee3f7fb93839b03c3fd0117d59426e7040e84ea7e`.
The tables below persist the delivery findings in the repository; the local raw
directory is not an uploaded/cloud artifact. Corpus fingerprints describe the
fixed measurement inputs, before the final documentation/help-text cleanup.

### Backup timings

Seconds, minimum–maximum of two repeats. Targets are decimal MB. `R` is the
unmodified repository corpus, `A` its amplified contents, `M` synthetic mixed.

| Profile | Target | v3 first | v4 first | v3 unchanged | v4 unchanged | v3 edit | v4 edit |
| --- | ---: | --- | --- | --- | --- | --- | --- |
| R | 64 | 0.084–0.088 | 0.318–0.351 | 0.075–0.078 | 0.131–0.139 | 0.074–0.081 | 0.229–0.229 |
| R | 256 | 0.079–0.082 | 0.281–0.308 | 0.077–0.077 | 0.125–0.139 | 0.075–0.082 | 0.221–0.227 |
| R | 1000 | 0.074–0.078 | 0.259–0.302 | 0.074–0.077 | 0.120–0.138 | 0.071–0.082 | 0.186–0.226 |
| A | 64 | 5.188–5.661 | 11.237–11.487 | 5.215–5.425 | 1.889–1.900 | 5.178–5.240 | 2.111–2.199 |
| A | 256 | 5.006–5.449 | 10.925–11.419 | 4.944–5.576 | 1.857–1.940 | 5.038–5.374 | 2.182–2.209 |
| A | 1000 | 5.089–5.559 | 10.794–11.528 | 5.066–5.394 | 1.856–1.936 | 5.027–5.484 | 2.132–2.276 |
| M | 64 | 11.857–12.603 | 18.721–18.847 | 11.813–11.828 | 2.375–2.379 | 11.757–11.775 | 3.169–3.334 |
| M | 256 | 11.132–12.598 | 18.179–18.517 | 11.826–12.209 | 2.454–2.636 | 11.513–11.598 | 3.142–3.216 |
| M | 1000 | 11.199–11.567 | 17.061–17.843 | 10.687–10.757 | 2.296–2.465 | 10.930–10.949 | 3.091–3.112 |

### Restore and cleanup timings at the retained 1000 MB requested target

Seconds, minimum–maximum of two repeats. Each full/selective restore is measured
separately; none is inferred from backup timing. Temporary restore storage was
explicitly redirected to the private disk-backed fixture.

| Profile | v3 full | v3 single | Older v4 full | Newer v4 full | v4 single | Confirmed cleanup | Retained restore |
| --- | --- | --- | --- | --- | --- | --- | --- |
| R | 0.318–0.410 | 0.014–0.016 | 0.336–0.372 | 0.326–0.391 | 0.022–0.023 | 0.381–0.451 | 0.352–0.441 |
| A | 10.812–12.618 | 1.026–1.031 | 11.535–12.215 | 11.911–12.290 | 1.179–1.200 | 3.512–3.692 | 12.014–12.362 |
| M | 15.023–16.294 | 1.873–1.891 | 14.213–16.101 | 14.717–17.346 | 1.917–2.032 | 6.071–6.848 | 14.441–16.744 |

### Transfer, operation, and private staging observations

Representative first-repeat byte counts below; corresponding repeats had the
same payload-object counts and staging peaks. All unchanged runs uploaded **zero
payload objects**, but uploaded control metadata. Every amplified/mixed edit
uploaded one complete 262,159-byte edited file, not merely its appended 15 bytes.

| Profile | Target MB | v4 baseline objects / provider calls | v4 baseline private peak | v3 baseline private peak | Unchanged uploaded/downloaded bytes | v4 single-file downloaded bytes |
| --- | ---: | --- | ---: | ---: | --- | ---: |
| A | 64 | 9 / 230 | 70,295,754 | 67,132,993 | 228,420 / 1,632,272 | 3,866,971 |
| A | 256 | 3 / 122 | 280,660,212 | 268,274,623 | 226,790 / 1,628,702 | 13,088,253 |
| A | 1000 | 3 / 122 | 287,671,712 | 513,424,101 | 226,837 / 1,628,939 | 13,463,643 |
| M | 64 | 9 / 230 | 132,289,822 | 98,121,850 | 228,429 / 1,632,299 | 34,812,560 |
| M | 256 | 3 / 122 | 529,003,024 | 392,417,291 | 226,793 / 1,628,711 | 137,153,477 |
| M | 1000 | 3 / 122 | 542,004,148 | 750,958,253 | 226,840 / 1,628,948 | 140,508,700 |

At 1000 MB, A first-run uploaded/downloaded 27,894,852 / 27,900,855 bytes;
M transferred 288,300,582 / 288,306,585. A edits transferred
1,638,035 / 3,042,766 bytes; M edits 1,642,785 / 3,047,522. The real repository
corpus used one object at every target, with a 1,279,703-byte v4 private peak and
1,069,704-byte v3 peak; its unchanged run transferred 11,332 / 80,562 bytes and its
single-file v4 restore downloaded 243,429 bytes.

All amplified/mixed newest snapshots referenced 537,133,071 physical member bytes,
including **524,288 obsolete member bytes**, at every target. Those obsolete
members remained in partly live archives even after confirmed newest-one cleanup.
The real repository trace retained 6,167 obsolete member bytes. These are not
reclaimable compressed-byte estimates or long-term compaction predictions.

First-run phase means at 1000 MB (seconds), from dedicated observations except
the explicitly labelled ending-boundary verification/receipt interval:

| Profile | Enumerate | Plan/hash | Private capture | Compression | Verify/receipt interval | Publish manifest | Provider upload | Provider download |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| R | 0.0005 | 0.0073 | 0.0107 | 0.0148 | 0.0291 | 0.0134 | 0.0201 | 0.0383 |
| A | 0.0096 | 1.1872 | 1.4446 | 3.0618 | 2.3491 | 0.0273 | 0.0856 | 0.0745 |
| M | 0.0097 | 1.1421 | 1.3882 | 7.6828 | 3.1344 | 0.0303 | 0.2205 | 0.0778 |

The verification interval includes receipt/journal ordering; it is not exclusive
CPU time. Provider intervals overlap other phases, and unlisted initialization,
local archive validation/hash, and journal work account for remaining wall time.

## Archive-target decision

**Retain the approximately 1 GB uncompressed requested target**, together with
the existing 1 GB staging budget and 1,000-file limit. This is a measured choice
for the normal configuration, not a promise of 1 GB compressed archives:

- Production constraints already limited the amplified/mixed v4 baseline to
  three objects at both 256 MB and 1000 MB. Moving to 256 MB saved only about
  2.4% of mixed private-peak/selective-transfer bytes in this trace, with no
  consistent repeat-level timing advantage.
- A 64 MB target reduced selective-transfer cost substantially, but raised v4
  baseline calls from 122 to 230 and objects from three to nine. It did not reduce
  observed obsolete member bytes in these edit/delete traces.
- The small unmodified repository used one object at every target. Fixed
  verification/control overhead dominates there; no target choice made v4 faster
  than v3 in that local case.

The default therefore prioritizes fewer remote operations while preserving
bounded, fully verified capture. Different latency, selection, file-size and
change-rate distributions can favor smaller groups; users must not interpret
these local fixtures as measured cloud throughput or a universal optimum.

## Pre-commit review record

Separate Standards and Spec reviewers rechecked the integration. Blocking
findings were resolved with regressions: skipped-link handling, abandoned cleanup
previews, takeover after legacy cleanup, configuration/journal overlap, exact
confirmation after legacy resume, and the integrated failure matrix. Both final
source re-reviews found no remaining verified blockers. Both final acceptance
reviews independently verified the 18 raw evidence files and receipt, approved
the documented scope/limitations and archive-target decision, and found no
remaining blockers for commit/issue closure after delivery to `master`.
This work does not authorize deployment.
