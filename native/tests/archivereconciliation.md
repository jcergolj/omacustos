# Archive continuation reconciliation (#51)

Resume rescans the current selection and hashes every member of each candidate
archive. Candidates come from the latest durable prepared or verified mappings,
not from the current enumeration or grouping order. Persisted root prefixes,
restore paths, and member paths remain reserved even after their source disappears.

A complete unchanged group is reused only after checksum-grade remote evidence:
matching content size and SHA-256 metadata, or a bounded staging download and hash
when the provider only supplies sizes. Missing objects are confirmed through a
successful parent listing; provider/authentication failures remain unfinished.
Changed, removed, excluded, missing, or corrupt groups are superseded by newly
prepared archives. Old objects are not referenced by the final index, and unchanged
groups retain their original identities without another upload.

An upload interrupted before verified checkpoint publication can be recovered
from its prepared record. That record alone never counts as verified work. Current
local hashes and positive remote content evidence must match before publishing a
new durable verified checkpoint and logical progress. Normal staging-budget,
oversized-file, cooperative-control, and storage-Waiting rules apply to these
verification downloads too.

## Integration coverage

`workerrecovery-test::archiveReconciliation` runs the real worker and subprocess
CLI with independently stored remote bytes. Its checksum and size-only rows cover:

- same-size local edits; additions, deletions, and new exclusions;
- reordered/removed roots and newly added roots with colliding names;
- file-to-directory and directory-to-file replacements;
- missing objects and same-size remote corruption;
- completed uploads interrupted before verification, including subsequent local
  edits, remote corruption, and missing remote objects.

Every row interrupts finalization again after reconciliation, resumes a second
time, verifies the same copy identity and exact current logical counts, and checks
that unchanged archives are uploaded only once. Superseded groups cannot reappear
from older journal records. Final indexes are loaded by the public restore
controller; restored bytes/hashes and the exact file count match the current
selection. No cross-copy reuse, deduplication, or historical repacking is involved.

Existing `archiveInterruptionRecovery`, `archiveReuseStorageWaiting`, and
`archiveControlsPreserveOnlyDurableProgress` regressions cover controls, checkpoint
failure, quota failure, bounded reuse downloads, and recovery on another staging
disk. Archive storage remains internally enabled until the #52 rollout.

```sh
cmake --build build -j 4
./build/workerrecovery-test archiveReconciliation
ctest --test-dir build --output-on-failure -j 4
```

These fixtures establish local correctness, not Proton throughput measurements.
