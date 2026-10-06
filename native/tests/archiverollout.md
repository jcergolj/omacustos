# Archive defaults and performance (#52)

## Rollout and deterministic remote-operation gate

Manual and scheduled requests use the same worker default: bounded v3 archives.
Recovery overrides that default with the durable identity's recorded format.
There is no user preference or saved-backup migration. Internal fixture overrides
remain available for historical v2 tests and the earlier single-archive seam.

`workerrecovery-test archiveRolloutDefaults` covers fresh manual and scheduled
requests for an existing saved backup, an unfinished v2 attempt resumed with the
new default, unchanged historical index bytes, and mixed-format public-controller
discovery. With the same 8,192 original bytes split among either 64 or 512 files
in separate directories, both runs transfer four archives and one index.
Verification/retention metadata stays under a constant envelope (30 commands,
including historical-copy discovery and remote directory creation); public
catalog selection uses at most eight metadata commands and one index download.
No fresh archive is downloaded by verification/retention. These are operation
counts at the real worker/subprocess CLI boundary, not a wall-clock CI gate.

Version-1 compatibility derives identity from a timestamped copy namespace
inside the configured backup folder (v1 has no embedded provenance). Every payload
reference must stay inside that exact copy; unscoped references and invalid copy
timestamps are rejected. The scheduled rollout row browses/restores an actual v1
index through the public controller. `recentbackupcopies-test
versionOneHistoryRemainsBrowsableRestorableAndDeletable` checks retention
eligibility, whole-copy deletion, and rejection of a foreign payload reference.

The full CTest suite retains control, crash, quota Waiting, source reconciliation,
Incomplete/success-gated retention, extraction integrity, mixed-format history,
owned staging recovery, and QML selection/download-cost coverage. Prepared-journal
publication now has a distinct phase, **Saving prepared work (not yet uploaded)**;
only durably verified members increment logical progress.
Final verification: `QT_QPA_PLATFORM=offscreen ctest --test-dir build
--output-on-failure -j 4` passed all **24 registered native/QML executables**;
the real-worker integration executable passed its full recovery/archive matrix.

## Opt-in resource and round-trip comparison

```sh
cmake --build build --target largebackup-benchmark -j 4
QT_FORCE_STDERR_LOGGING=1 build/largebackup-benchmark --compare 8192 268435456 developer
QT_FORCE_STDERR_LOGGING=1 build/largebackup-benchmark --compare 8192 268435456 mixed
```

`--compare [files] [original-bytes] [developer|mixed]` extends the existing
opt-in benchmark; the original large individual-file continuation benchmark
remains available with its original positional arguments. Comparison creates
owned disk-backed source, remote, staging, journal, and restore trees and removes
them on completion. Both formats consume identical generated inputs. Developer
files contain code-shaped records with changing module/record identifiers; mixed
input substitutes deterministic incompressible binary content in half the files.
These are synthetic representative workloads, not measurements of a real user
tree. Scale the arguments for a larger disk/resource investigation.

The local provider retains independent remote bytes and verifies SHA-256.
All logical files restore through the production engine and their bytes/size/hash
are checked against the source. Archive groups use a 16 MiB uncompressed target
and a 64 MiB staging budget so several groups are exercised with modest disk use.
JSON output records scanning/selection, snapshot preparation, compression,
upload, verification, prepared/verified checkpointing, final-index work, restore,
operation counts, uploaded bytes, sampled staging, and process memory high-water
marks. Staging is sampled at phase transitions and periodically, including
simultaneously retained immutable snapshots and compressed outputs, not just
indexed source sizes. Restore sampling observes private download/extraction
workspaces. Sampling can miss short peaks and adds instrumentation overhead;
the deterministic quota/peak-retention fixtures remain the staging correctness
gate. Current RSS is sampled separately for each phase, backup interval, and
restore interval, resetting interval peaks for each format. Samples may miss
short peaks and current RSS includes reusable allocations/cache retained by the
process. The additional `rss_process_high_water_bytes` fields are a
**cumulative process high-water mark**, including the preceding
individual-file baseline when the archive row is produced; it is not isolated
per-phase allocation or a standalone archive RSS figure. Timings are single-run
observations, not p95 or throughput guarantees.

Each comparison also recompresses the largest produced archive's exact tar
stream with gzip levels 1, 3, and 6, streaming bounded buffers. Each level must
round-trip to the same uncompressed SHA-256. Reported time includes inflating the
original archive, hashing the tar stream, and compression; validation afterward
is outside that timing. The shipping level-3 default is unchanged.

### Local fixture measurements

Measured on an Intel Core i5-9600K (six physical cores), 46 GiB RAM, Linux,
GCC 16.2.1 / Qt 6.11.2 / zlib, Btrfs disk-backed workspace,
8,192 files / 256 MiB per profile. These exercise local copying and
metadata hashing, **not actual Proton throughput or network completion time**.

| Profile / format | Backup | Restore | Upload + verification calls (each) | Restore downloads | Uploaded bytes incl. index |
| --- | ---: | ---: | ---: | ---: | ---: |
| Developer / individual | 13.48 s | 22.96 s | 8,193 | 8,192 | 271,620,336 |
| Developer / archives | 5.87 s | 26.60 s | 17 | 16 | 22,940,076 |
| Mixed / individual | 14.22 s | 22.86 s | 8,193 | 8,192 | 271,620,336 |
| Mixed / archives | 9.07 s | 27.09 s | 17 | 16 | 150,961,580 |

Sampled simultaneous staging: individual 32,768,128 bytes, developer archives
17,926,277 bytes, mixed archives 25,924,462 bytes; retained backup staging after
completion was zero. End-of-run cumulative RSS was approximately 52 MiB after
individual restore and 66 MiB after archive restore. Local full restore was
slower for archives in these observations: extraction/integrity work is a real
trade-off, even while payload-object operations fall dramatically.

Sampled current RSS peaks (backup / restore): developer individual
51,359,744 / 51,052,544 bytes, developer archives 65,310,720 / 65,572,864 bytes;
mixed individual 51,388,416 / 51,142,656 bytes, mixed archives
65,253,376 / 65,511,424 bytes. Sampled restore staging was 32,768 bytes for
individual files, 17,926,149 bytes for developer archives, and 25,924,334 bytes
for mixed archives. Final-index workspace samples were 3,185,008 bytes for
individual indexes and 4,656,504 bytes for archive indexes.

| Profile (17,040,384-byte tar) | Gzip level | Compressed bytes | Recompression incl. inflate |
| --- | ---: | ---: | ---: |
| Developer | 1 | 1,131,626 | 96.5 ms |
| Developer | 3 | 1,148,933 | 107.4 ms |
| Developer | 6 | 1,071,570 | 172.4 ms |
| Mixed | 1 | 9,135,721 | 248.8 ms |
| Mixed | 3 | 9,147,118 | 245.8 ms |
| Mixed | 6 | 9,152,087 | 294.0 ms |

Compression ratios and speed do not improve monotonically on every workload.
Here level 6 saved about 6.7% against level 3 for developer content at about 61%
more recompression time, but produced a slightly larger mixed archive at about
20% more time. Level 1 happened to be smaller than level 3 on both synthetic
groups, faster on developer content and similar in time on mixed content.
This is evidence of workload dependence, not approval
to change the agreed default or add adaptive compression/tuning controls.

See `MANUAL.md` for approximately 1 GB normal grouping, selective-download
amplification, whole oversized-file staging, Waiting/Resume, compatibility,
gzip level 3, and full-copy daily behavior. Incremental backups, cross-copy
deduplication, and custom oversized-file splitting remain outside this rollout.
