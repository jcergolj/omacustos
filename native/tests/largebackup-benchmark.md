# Issue #43: large-backup resource and continuation evidence

## Reproduction

Build the native CMake targets, then run on a disk-backed filesystem:

```sh
cmake -S native -B build/issue-43 -DCMAKE_BUILD_TYPE=Debug
cmake --build build/issue-43 -j4
QT_FORCE_STDERR_LOGGING=1 build/issue-43/largebackup-benchmark 877172 95323650798
ctest --test-dir build/issue-43 --output-on-failure
```

`largebackup-benchmark` is opt-in, not part of CTest. It creates **877172 regular
source paths**, with exactly **95323650798 selected bytes**, in groups of 1000.
Generated hard-linked seeds reduce physical source storage; **each selected file
is still opened, copied, hashed, uploaded through the measuring provider, and
verified by the real backup engine**. Staged payload bytes are actually written
to disk and the provider reads/hashes them. The generated workload is private to
the benchmark and removed on normal completion.

The measuring provider retains remote content identities, rather than another
95.3 GB local payload copy or live Proton data. Thus this is a representative
engine resource/continuation run, not a real-cloud throughput or CLI-memory run.
The public worker regressions use a real subprocess CLI fixture with independently
stored remote payloads and verified manifests for end-to-end behavior.

## Recorded run: 2026-10-05

Environment: Linux, Btrfs disk-backed working directory, Qt 6.11.2, Debug build.
`/tmp` was a RAM-backed tmpfs; no backup payloads were staged there.

The first invocation deliberately interrupted after **10000 verified files**, then
re-scanned and continued `/copy/same-copy`. At approximately 900 seconds, the
execution harness's runtime limit stopped that invocation; the durable journal
contained **820000 verified files**. No final manifest had been claimed.

The benchmark's optional third argument resumes an existing generated workload:

```sh
QT_FORCE_STDERR_LOGGING=1 ./largebackup-benchmark 877172 95323650798 \
  ./largebackup-benchmark-WllAdU
```

That command was run from `build/issue-43`. The measuring provider's virtual remote
identities were rehydrated from the previously observed journal for this harness
recovery. This is an explicit fixture assumption, not evidence that a checkpoint
alone proves cloud content. Separate subprocess tests remove/corrupt remote bytes
and prove the actual worker re-verifies or re-uploads before reusing them.

The resumed resource run completed successfully:

| Measurement | Result |
| --- | ---: |
| Verified files in final manifest | 877172 |
| Verified selected bytes | 95323650798 |
| Cumulative payload uploads | 877172 |
| Batches | 878 |
| Maximum files in a prepared batch | 1000 |
| Peak prepared payload bytes | 108672000 (108.7 MB) |
| Default payload budget | 1000000000 (1 GB) |
| Selection/checkpoint RSS high-water, resumed process | 1371062272 bytes (1.28 GiB) |
| Execution RSS high-water, resumed process | 1563324416 bytes (1.46 GiB) |
| Final-manifest RSS high-water | 3676766208 bytes (3.42 GiB) |
| Final local manifest size | 378938645 bytes |
| Resumed invocation elapsed time | 411756 ms |

All selected payloads were verified, the final manifest passed engine validation
and provider verification, and previously checkpointed files were not uploaded
again on continuation. The benchmark checked exact file/byte/upload counts and
both batch limits before returning success. Its generated workload was cleaned up.

## Metadata assessment

The payload peak is independent of total selected bytes, but **metadata is still
O(file count)**. Selection strings, durable content/mapping identities, collision
sets, and the measuring provider's own metadata contribute to execution RSS.
Manifest JSON construction and validation account for the substantially higher
finalization peak. Per-directory provider metadata can exceed a batch when a
remote directory has many children; batch lifetime bounds cache retention, not
the size of one CLI listing. These numbers are process `VmHWM`, not service-cgroup
memory, system page cache, or a proof that arbitrary metadata fits in 1 GB.

The selected data exceeds the normal 1 GB staging budget by approximately 95×.
The 1000-file limit makes this workload's actual payload peak about 109 MB. The
small-capacity regression separately proves total input can exceed available
payload capacity: preflight free-space and per-user-quota fixtures fail writes
deterministically without filling the machine's filesystem.

## Routine public-boundary regressions

`workerrecovery-test` executes the real worker, engine, durable stores, and a CLI
subprocess. It covers:

- bounded bytes, 2005 empty files across file-count-limited batches, oversized files;
- write/quota exhaustion despite free filesystem space, and insufficient space
  for one file followed by recovery on another staging disk;
- same-copy recovery after killed transfers/finalization and checkpoint publication
  failure; partial folder commands and manifest failures;
- local same-size edits, additions, deletions, exclusions, root changes, missing
  remote payloads and same-size remote corruption, including size-only metadata;
- Pause/Resume/Cancel, scheduler invocations, crash before control acknowledgment,
  cancelled namespace history, and a fresh copy after cancellation;
- owned orphan cleanup with active/unrelated/source data protected;
- reserved manifest directories, file-to-directory changes, and source-root
  namespace collisions without lost or duplicate restore entries;
- exact individual source-read failures, verified Incomplete manifests, and
  protection of older successful copies.

Existing dashboard tests exercise control targeting/visibility, waiting reasons,
staging-setting drafts, and navigation while work is active. Existing immutable
snapshot, recursive-transfer, restore, manifest, reliability, and retention
regressions remain in the full native suite.
