# Single-archive acceptance coverage (#46)

The primary seam is `workerrecovery-test`: it launches `omacustos-worker`, with
the production engine/staging/provider/continuation/run-store path and the offline
`continuation_cli.py` subprocess fixture. Remote archive bytes are stored independently
of staging. Restore uses `BackupRestoreController` with the real subprocess provider
against those same worker-produced objects.

| #46 acceptance | Evidence |
| --- | --- |
| Real worker through public restore controller | `singleArchiveRoundTrip`, `singleArchivePreparationRace`, archive row of `sourceFailuresFinalizeIncompleteAndProtectSuccessfulCopies` |
| Valid gzip-3 tar; ordinary remote objects | Python tar/gzip interoperability and independent gzip-3 deflate comparison; two uploads (archive + index), no per-member uploads |
| Logical hashes describe archived bytes under source races | Preload fixture deterministically changes remaining source contents or replaces the pathname during snapshot writes; resulting index sizes/hashes and controller-restored bytes match exactly |
| Explicit v3 provenance/archive/logical index | Worker-produced index loads as v3; entries resolve shared archive identity/location/compressed size/hash plus source/restore/member/size/hash |
| Transactional index validation | `validatesArchiveIndexTransactionally`: missing/unused/duplicate archive references, duplicate locations/source/restore/member identities, path safety, manifest/source/member/restore collisions, provenance, sizes/hashes, wrong remote and parent conflicts; failed loads expose no entries/provenance |
| Archive and final-index content verification | Both metadata-policy rows round-trip; `singleArchiveVerificationFailure` rejects encrypted storage size, archive checksum damage, and final-index checksum damage; unfinished, never successful |
| Picker needs no archive download or per-file metadata | 84 logical files; browse downloads one index; at most three metadata operations; backup/retention never downloads fresh archive bodies |
| Safe selective extraction and atomic placement | Three selected members share one download; hidden/empty files restored, nested/unselected/index-named source remains absent; outer/member size/SHA-256 checks precede atomic placement |
| Unsafe/malformed/truncated/checksum rejection | `singleArchiveRejectsDamage`: archive/member size and checksum, truncated gzip/tar, traversal, absolute member, indexed symbolic/hard links, duplicate, missing, unindexed member, invalid gzip/tar, gzip CRC, missing tar end blocks, appended tar after an early end marker; existing destination preserved; no completion |
| Legacy compatibility and mixed dispatch | Same worker fixture makes a subsequent default v2 copy; controller browses both formats, restores legacy bytes, then reselects v3; `archiveCopyDeletionRetainsWholeCopyScope` deletes only the archive copy and retains its legacy sibling; existing v1/v2 manifest and engine/controller regressions remain in full CTest |
| Source selection, logical counts, async execution/progress | Existing scan/mapping/staging/worker publication path reused; nested, hidden, empty, exclusions, excluded symlink, reserved manifest source name, accurate 84 logical-file result; source-read failure publishes restorable v3 Incomplete and preserves older success |
| Supplied dependencies | Required linked LibArchive/ZLIB CMake targets and explicit `libarchive`/`zlib` PKGBUILD + .SRCINFO dependencies |

Additional working-space gates are `singleArchiveStorageFailure` (conservative
single-copy allowance, snapshot quota and archive-output quota). They assert Waiting,
unfinished identity, no verified index, no false verified files, and released staging.

Focused commands:

```sh
cmake -S native -B build
cmake --build build -j2 --target workerrecovery-test backupmanifest-test
build/workerrecovery-test singleArchiveRoundTrip singleArchivePreparationRace singleArchiveStorageFailure singleArchiveVerificationFailure singleArchiveRejectsDamage sourceFailuresFinalizeIncompleteAndProtectSuccessfulCopies
build/backupmanifest-test
```

Final integration gate: full `cmake --build build -j2`,
`ctest --test-dir build --output-on-failure`, and `git diff --check`.

Verified on 2026-10-06: full configure/build succeeded; all **24 CTest suites**
passed (109.08 seconds), including the real worker, public restore controller,
legacy recovery, copy management, retention, and QML suites. The focused manifest
suite passed **67 cases**, and all focused archive damage/round-trip/storage/race
cases passed. `git diff --check` passed. The pre-existing relocated build cache
required an initial `cmake --fresh -S native -B build` regeneration.

This slice intentionally has no production-default switch, storage-mode UI,
multi-archive preparation, oversized-file exception, or archive continuation reuse.
Those contracts are reserved for the dependent tickets and rollout #52.
