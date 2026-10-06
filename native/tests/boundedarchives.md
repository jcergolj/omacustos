# Bounded archive groups (#47)

The internal worker seam `OMACUSTOS_INTERNAL_BOUNDED_ARCHIVES=1` creates
version-3 archive copies. Production rollout remains the #52 ticket. The normal
payload target is 1,000,000,000 bytes; `OMACUSTOS_INTERNAL_ARCHIVE_TARGET` permits
small equivalent boundaries in the real-worker/subprocess fixture.

Normal preparation accounts for immutable snapshots and compressed output at
the same time. The conservative compressed allowance includes 1% expansion,
8,192 bytes per member for tar/PAX overhead, and 65,536 bytes of stream overhead.
Groups shrink to honor the configured staging budget. Related direct-directory
members stay together where they fit an ordinary group; large directories split,
and adjacent tiny directories share groups. Selected roots are not combined.

A file exceeding the payload target or normal working allowance stays whole in
a single-member archive. Preflight requires its snapshot plus conservative
compressed allowance, independently of anticipated compression. Insufficient
space reports required bytes and the oversized exception. Normal and oversized
write/quota/flush errors retain unfinished work and actionable storage Waiting.

Archive objects live in a reserved `.omacustos-archives` directory, separate from
source member/restore identities. Each group is packed with gzip level 3, uploaded,
verified, durably checkpointed (archive size/hash/membership once per batch, with
linear per-file identity/evidence records),
and then its snapshots and compressed output are released. Prepared checkpoints
never authorize reuse. Resume/reconciliation is implemented by #50/#51.

## Regression evidence

`workerrecovery-test boundedArchivesRoundTrip` exercises five workloads with a
200,000-byte normal budget: incompressible total input above capacity, target
boundaries and related directories, many tiny directories, multiple roots and
reserved/hidden names, and a whole 300,000-byte oversized file. The existing
quota fixture measures **actual retained files throughout the owned attempt**,
including both snapshots and gzip outputs, and enforces the normal capacity.
Only the oversized row permits a larger working allocation.

Each worker-produced copy is restored through the public controller as a
cross-archive selection, a folder selection, and the whole copy. Tests compare
exact bytes/paths, reject unselected destination content, and assert one download
per required archive. Existing single-archive damaged-content tests continue to
cover malformed archives and unsafe members.

`boundedArchivesStorageWaiting` covers conservative oversized preflight after a
verified group, snapshot quota, compressed-output quota, and durable-checkpoint
quota, and write/stream-flush input/output failures after a verified group. The
flush fixture injects the libarchive close-callback error contract (the normal
unbuffered QFile sink generally has no remaining buffered bytes to flush).
The interrupted cases with verified work independently validate preserved remote archive bytes
against the journal's verified checksum/membership. No final index or success is
published on these interruptions.

Metadata and final index memory remain proportional to logical file count; this
is a temporary **payload** bound, not a constant-memory assertion. Each fresh run
still uploads a full copy. Selecting a file requires downloading its containing
archive, although only selected indexed members reach the restore destination.

## Incomplete copies and mixed-format retention (#49)

`sourceFailuresFinalizeIncompleteAndProtectSuccessfulCopies` exercises source
disappearance during preparation for individual files, a single archive, and
bounded archives with either legacy or archive successful history. The surviving
file is exposed by the verified Incomplete index and restored through the public
controller with exact bytes; the failed path is absent from recoverable entries.
The worker records its exact source path, reading phase, and reason, and retains
the older successful copy despite a retention count of one.

`archiveSourceFailuresWithoutSurvivors` proves that source failure without any
verified recoverable files is terminal Failed, preserves successful history,
and releases staging. Existing archive storage/verification interruption tests
cover unfinished Waiting/Retrying instead of false partial finalization.
Catalog, retention/discovery, and recent-copy management fixtures cover shared
archive verification, mixed-format browsing, and exact whole-copy deletion.

```sh
cmake --build build -j2
build/workerrecovery-test boundedArchivesRoundTrip boundedArchivesStorageWaiting
ctest --test-dir build --output-on-failure
```
