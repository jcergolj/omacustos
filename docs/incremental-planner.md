# Read-only incremental planner (#57)

> Historical component delivery record. Its rollout-status statements are
> superseded by [the default-worker integration](incremental-default-rollout.md).

`IncrementalPlanner::plan` in `native/src/incrementalplanner.{h,cpp}` implements
source capture and deterministic comparison against exactly one previous v4
snapshot. It is deliberately not wired into today's full-backup worker or UI:
the [repository contract](incremental-repository.md) still gates default rollout
on restore, GC and release validation. #58 adds a separate executor using the
explicitly approved operational single-writer/manual-offline-handoff model, not
remote fencing.

## Inputs and capture

Supply the stable set UUID, a **completed** scanner's selected regular files with
their stable relative restore mappings, and its scan issues. Any scan issue fails
the whole plan: a missing/unreadable source root must not masquerade as intentional
selection removal. Existing scanner inclusion/exclusion and collision mapping
remain the caller's responsibility. Empty sources with no scan issues explicitly
represent an empty selection. Supply the full previous manifest and its selected
snapshot ID, or neither for the first baseline. A missing/corrupt expected manifest
is an error, not permission to silently start another baseline. Legacy copies are
not content-reuse inputs; the first v4 baseline is fresh.

The planner uses the v4 validator with the requested set/snapshot before looking
up any content. It rejects unsafe/duplicate restore destinations and file/parent
conflicts before opening sources. Absolute source paths must be clean. Linux
`openat` walks every path component with `O_NOFOLLOW`, so neither parent nor leaf
symlinks are followed. Only regular files are read. Nonblocking opens avoid waiting
on a source replaced by a FIFO.

Every selected file is read and SHA-256 hashed, even if its size and nanosecond
mtime match the previous snapshot. Reads use a 64 KiB chunk, one open file at a
time; no whole-file buffer, temporary payload, remote operation, configuration
write or staging disk allocation occurs. Metadata still scales with file count.
The bounded hash pass applies equally to files larger than the staging budget.

Capture records descriptor identity, size, mode, nanosecond mtime and ctime before
reading. After hashing it rechecks both that descriptor and a freshly no-follow
opened source pathname, requiring matching device/inode, size, full mode, mtime,
ctime and observed byte count. Read errors, content/metadata changes, pathname
replacement, unsupported size/mtime range or cancellation fail transactionally:
no partial files, removals or archive requirements are returned, and the previous
successful-check time is preserved. Each file has its own capture instant; this
does not promise a filesystem-wide point-in-time snapshot or protection against a
malicious filesystem falsifying stat/hash observations.

## Deterministic result

`files` are sorted by case-sensitive restore path, and each has a disjoint logical
change classification:

- **Added:** destination absent from the previous logical state (including rename
  destinations and newly selected files).
- **Changed:** existing destination with different size/SHA-256, permission bits
  0777, or nanosecond mtime.
- **Reused:** destination, bytes and supported restore metadata unchanged.

Content reuse is independent of that classification. Every matching `(size,
SHA-256)` from the previous snapshot's **logical entries** can supply an archive
and member, including for an Added rename/duplicate or Changed metadata-only file.
Choose the lowest `(archive ID, member)` tuple, independent of manifest or scan
order. Multiple files can reference that same content. Do not search an archive's
obsolete physical members, other snapshots, local history, or other sets. New
duplicate content not present in the previous snapshot remains whole-file upload
work; this planner does not introduce within-run payload packing/deduplication.

`requiresUpload()` is true only without a reusable archive/member. Upload candidates
are whole new/changed file contents, never unchanged archive neighbors. `removed`
lists old destinations absent from the new selection, sorted independently.
`requiredArchives` contains exactly the reused archive descriptors sorted by ID,
preserving complete physical inventories and original verification evidence,
including members no longer logically referenced. New object IDs and packing are
the later uploader's responsibility.

`snapshotRequired` is false only with a previous snapshot whose complete logical
state (paths, content and supported metadata) is unchanged. An initial empty
baseline requires a snapshot; a later empty-to-empty check does not. Removing the
last file requires a saved empty state. Physical reference choice alone is not a
logical change and does not create another snapshot.

## Successful checks, saved changes and later execution

The request supplies `checkedAt` and the prior `lastSuccessfulCheck`. Reject an
invalid or regressing check time. Successful local planning returns that check time
separately from `lastSavedChange` (the previous manifest's creation time, invalid
before the first saved state). A changed plan **does not advance last saved
change**: only verified publication can do that. Both fields are exposed in the
result so a later worker/UI can distinguish checks from saved states.

For an unchanged plan, the future writer must still perform the contract's remote
reuse health checks, then durably record the successful check without creating a
manifest or consuming retention. The planner itself writes no check history and
does not claim current remote integrity or authorize cleanup. Its successful-check
timestamp is a candidate local observation, not an end-to-end backup success until
those later checks pass. Existing full-backup UI timestamps remain unchanged.

Because the planner keeps no staged payloads, a later executor **must not upload
directly from these source pathnames**. Recapture each upload candidate into the
existing private, bounded staging workspace, check captured size/hash/mode/mtime
against the plan, and replan on mismatch. Preserve immutable staged bytes through
packing/upload/verification/checkpoint release, as required by the repository
contract. A successful local plan does not relax staging capacity, original
verification evidence, fresh reuse health checks, source-change checks or manifest
publication ordering. This seam cannot authorize the later remote operations.

## Automated validation

`incrementalplanner-test` uses real local files and nanosecond metadata, covering
baseline/unchanged and empty states, additions/edits/removals, selection changes,
renames/duplicate content, deterministic matching/ordering, metadata-only changes,
same-size/same-mtime edits, physical-only inventory members, same-set validation,
unsafe mappings, scan failure, symlinks, missing files, in-read writes/truncation/
pathname replacement and cancellation. Race tests mutate sources at the planner's
cooperative read checkpoints, without sleeps or cloud calls.

### Validation record (2026-10-20)

- `cmake -S native -B build` and `cmake --build build -j 4`: passed
  (existing optional Vulkan/QML-policy warnings only).
- `build/incrementalplanner-test`: **20 passed, 0 failed**, including capture
  failure/recovery boundaries and cancellation. Planner and manifest targeted
  CTest run: **2/2 passed**.
- `ctest --test-dir build --output-on-failure -j 4 -E '^workerrecovery-test$'`:
  **25/25 passed**, 85.23 seconds, including legacy restore/retention and UI.
- `build/workerrecovery-test`, separately with a 360-second external allowance
  because of its previously documented 180-second CTest timeout: **139 passed,
  0 failed**, 173.619 seconds. Its configured CTest timeout remains unchanged.
- Pre-commit independent staged-scope reviews against `e20ef90`: **Standards:
  no blockers; Spec (#57): no blockers**. Both reviewers also reran the planner
  and manifest CTest targets successfully. `git diff --cached --check`: passed.
- No remote backup mutation or cloud test was performed.
