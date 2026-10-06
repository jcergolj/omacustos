# Technical Notes

This document records implementation details that are useful for maintainers
and advanced users but are not needed for the quick start.

For everyday instructions, see the [README](README.md) and [user manual](MANUAL.md).

## Native And QML Structure

CMake compiles shared implementations once into three static targets:

- `omacustos-core`: storage, providers, configuration, execution, and systemd
  helpers, linked only to Qt Core. The worker and installer use this target.
- `omacustos-controllers`: UI-facing controllers and asynchronous operations,
  linked to the core target and Qt Concurrent.
- `omacustos-theme`: desktop palette integration, linked to Qt Gui.

The application and native tests link these same targets instead of compiling
separate copies of their sources. Add shared implementations to their library
target; `omacustos_add_test` registers tests against the appropriate target.

`Main.qml` composes the window, navigation, dialogs, and notifications.
`Dashboard.qml` presents saved sets and recent runs and emits navigation requests.
`BackupEditor.qml` owns its draft fields and controller synchronization.
`RestoreSelection.qml` owns restore selection arrays, lookup, counts, folder
ancestry/counts, default/customization flags, copy identity, and selection snapshots.
Its operations reconcile refreshed paths, toggle files, select folders/all files,
clear, and save/restore snapshots. `RestorePanel.qml` owns destination, focus,
scrolling, and per-context presentation state, and forwards controller changes to
the selection owner. Restore opens in a dedicated scrollable screen, like the
backup editor; closing it preserves its state, and completion resets the panel
and returns to the dashboard. The top-right three-dot menu contains backup-set
import and export actions.
These components receive controllers and `UiStyle` explicitly. `FormCard` gives
backup and restore forms the same surface, padding, and typography.
`TransferProgress` shares the status, file-count text, and progress bar between
backup cards and the active restore card. Restore progress counts successfully
restored files, and remains visible across navigation. `ActionButton`
and `PreviewGroup` also provide shared presentation, inheriting the window's live
palette. Window state aliases and existing object/accessibility identifiers keep
the dashboard test interface usable across the extraction.

## Local State

OmaCustos stores user state under `~/.config/omacustos`:

```text
omacustos-backup.json
omacustos-backup-runs.json
omacustos-backup-cleanup.json
omacustos-browser-links.json
continuations/<sha256-of-exact-copy-path>/
```

The first file contains named backup configurations. Run history and pending
cleanup decisions are stored separately. Discovered remote backups are not written into
the local schedule or backup queue, so reinstall discovery never reactivates an
old schedule.
Browser links are an optional, bounded cache of private Proton Drive folder URLs,
keyed by the exact copy path. Losing this cache only requires resolving links again.

Backup sets are the sole in-memory source/destination representation. Legacy
`source_directory`/`remote_root` configuration is converted to the `default` set
on load. Every save writes the modern `sets` array, including an explicit empty
array, while preserving `proton_binary`. This migration only changes local state.

Run-state and cleanup-state stores parse supplied JSON bytes transactionally:
malformed documents or failed reads retain the entire last-good snapshot.
Dashboard polling reads each file once, compares the captured contents, and only
parses changed bytes. Missing files clear their snapshots; present empty files
are malformed. Refresh errors remain visible until a successful retry, while
remaining-time countdowns continue independently of state changes.

`BackupRunStore` owns durable manual enqueue, schedule catch-up, interrupted-run
recovery, prerequisite deferral, running publication, progress, and outcome
updates. Each mutation briefly locks `omacustos-backup-runs.json.lock`, reloads
the latest queue, and atomically saves the changed state. Waiting transitions
are saved before the worker skips that backup, even when every ready backup waits.
Progress and final outcomes update only the matching running attempt (backup ID,
copy path, attempt count, and scheduled time), preserving other queued work.

The worker separately holds `<config-path>.worker.lock` and
`omacustos-backup-runs.json.worker.lock` throughout execution. The latter excludes
workers using different config files in the same state directory without blocking
manual enqueue during transfers. It reloads the queue between backups to pick up
new requests, attempting each backup at most once per invocation. The progress
writer is joined before outcome publication, so pending samples cannot overwrite
a finished attempt. Executable setup supplies the system prerequisite probe;
the battery fixture exercises the same worker execution path with deterministic
AC-power state.

Modern running records carry `unfinished`, `control_request`, cancelled-copy
history, and staging-root history. Retry count is separate from copy identity.
Recovery/Resume retain the exact unfinished copy path; fresh requests after a
terminal outcome allocate a new namespace. Older records/configurations remain
readable. Pause/cancel decisions are merged under the run-store lock, are checked
cooperatively during scan/copy work and approximately every 100 ms during CLI
commands, and dominate late progress/final-result writes. Linux CLI children have
a parent-death signal; Pause/Cancel kill their process group. User-paused work is
never scheduler-ready. Authentication defers for 60 seconds; transient transfer
failures retain the copy and use exponential retry delay, capped at one hour.

`BackupContinuation` stores a versioned copy/root-identity header and atomic,
sequenced batch journals. Prepared mapping records are committed before transfer
and never authorize reuse. Verified records are committed after remote verification
and before payload release. Commits use `QSaveFile` without direct-write fallback
and sync the containing directory. Journal writes scale with a batch, rather than
rewriting every verified item at each checkpoint. Source/remote/restore mappings
remain stable across root reordering, additions, reserved names, and file/directory
changes. Rescan creates a new final manifest from the current selection; stale
remote bytes outside that manifest are not restore entries.

Copy management and import acquire `omacustos-backup-runs.json.management.lock`
before worker exclusion and release it last. A newly launched worker waits at
that gate, then acquires worker exclusion and releases the gate before running.
Thus requests queued during management execute after it finishes, including
manual-only backups with no timer. A competing worker still exits immediately
when another worker holds exclusion.

## Remote Layout

Each copy is stored independently below:

```text
<remote-root>/<computer>/<backup-name>/<copy-id>/
```

Every copy has a manifest containing its computer, backup identity and name,
copy, timestamp, completion state, expected items, failed items, paths, sizes,
and checksums.
New manifests also preserve an `issues` array containing each failed source path,
phase, and reason. The existing `failed` path list remains compatible with older
readers. An incomplete manifest can list an attempted path in both `expected`
and `failed`; a verified entry cannot also be listed as failed.
This provenance keeps similarly named backups on different computers distinct.

`BackupManifest` owns persisted entry and provenance types, version selection,
completion rules, serialization, and validation. Execution passes a
`BackupManifestDraft` containing verified payload observations, expected/failed
items, and issues; it does not construct JSON or claim a persisted completion
state. Writes pass through the same validation boundary as loads before atomic
replacement. Complete version-2 copies must contain exactly the expected entries
and have neither failed items nor issues. Incomplete copies retain only verified
entries, including when a folder transfer reports failure after transferring all
payloads. Older version-1/version-2 manifests remain readable, including those
without an `issues` field. Failed loads expose no partial entries or provenance.

The interface calls each saved configuration a **backup** and each run's output
a **copy**. Internal `BackupSet` types and persisted `sets`, `set_id`, and
`set_name` fields retain their existing names for compatibility with saved
configurations, run history, cleanup decisions, and remote manifests.

Legacy `required_volumes` settings are ignored on load and omitted on save;
backups no longer wait for external drives to be mounted.

Backup-set exports use a version-1 JSON document with `application: "omacustos"`
and a `sets` array. They contain the saved set definitions, without the CLI
executable, run history, cleanup decisions, files, or credentials. Import validates
the whole document before replacing the set list and preserves the local CLI
executable setting. Import is blocked while a worker or queue update is active.

Exclusions containing a path separator match that path and its descendants.
Bare names such as `node_modules` match directory names at every depth; regular
files with the same name remain included. Matching is exact and case-sensitive.
Excluded symbolic links and unreadable paths do not mark a copy incomplete.
Rules are normalized once per scan. Backup selection prunes excluded directory
subtrees; the interactive preview still traverses them to report individual
excluded files.

Folder traversal includes hidden regular files and hidden subdirectories, whether
the selected root itself is hidden or visible. The same exclusion rules apply to
hidden content. Symbolic links are never followed; hidden links are reported as
skipped unless excluded. Restore verification reads payload paths from the
manifest directly, so hidden payloads do not depend on provider directory listings.

## Provider Behavior

### Internal Single-Archive Slice (#46)

`BackupOptions::singleArchive` enables the small-copy archive execution seam.
The worker exposes it through `OMACUSTOS_INTERNAL_SINGLE_ARCHIVE=1` for integration
fixtures and development. Default rollout is reserved for #52; it is not a saved
storage-mode preference. The slice prepares one archive for a small selection,
using the existing source scan, collision mappings, exclusions, private disk
staging, progress publication, provider operations, and final-index verification.
Selections beyond the conservative single-archive working allowance enter Waiting;
bounded multi-archive grouping and oversized-file handling follow in dependent work.

The working allowance includes immutable source snapshots, compressed output,
tar/gzip overhead, and the existing filesystem metadata reserve. Snapshot hashes
are checked again while packing, so logical sizes and SHA-256 values describe the
archived bytes even when source contents or pathnames change during preparation.
Packing uses linked **libarchive** and **zlib**, with gzip level **3**. Both are
explicit CMake and Arch package dependencies; scheduled execution requires no
external tar/gzip installation.

Version-3 manifests retain version-2 provenance and completion rules, add an
`archives` table (`id`, `remote`, compressed `size`, `sha256`), and map each logical
entry to an `archive` and relative `member`, alongside its source, restore path,
logical size, and SHA-256. The table is authoritative; full member sets are derived
from the validated entries rather than persisted as a competing index. Validation
rejects missing/unused/ambiguous references, duplicate source/member/destination
identities, file/directory conflicts, unsafe mappings, and payload/index name
collisions before publishing any entries or provenance. Loads remain transactional.
Version-1 and version-2 manifests retain their existing loading and restore paths.

Catalog verification dispatches from the loaded entry format and verifies each
archive's content metadata once. It downloads only the index for browsing, never
the archive body. Fresh archive uploads retain the existing content-size/checksum
policy; encrypted storage size is not evidence, and a body download is not a new
unconditional requirement. Both the archive and final index must pass verification
before success. Source failures can produce a verified, restorable Incomplete copy.

Restore groups the captured selection by archive identity, downloads each required
archive once, checks its compressed size/SHA-256, validates the complete gzip stream
(including CRC and trailer) and tar end blocks, and streams selected regular members
into private staging. Unselected members are read for structural validation, never
extracted. Unsafe, duplicate, linked, unindexed, or missing members fail before
destination placement. Selected sizes/hashes must pass before the existing
symlink-checked `QSaveFile` atomic replacement path commits bytes. Downloading one
small file can therefore require the **whole compressed archive**; no permanent
archive cache or incremental daily-copy behavior is implied. Fresh runs remain full
copies. Completed earlier files retain the existing partial-restore semantics.

The continuation identity records `payload_format`, defaulting to 2 for older
journals, so changing the internal seam cannot mix payload formats in an unfinished
namespace. A replacement worker follows the recorded format even when its internal
archive flags differ. Archive journals preserve whole archive identities, compressed
size/SHA-256, source/member mappings, and prepared versus verified state. Each atomic
batch record contains only the current group, never a rewritten checkpoint history.
Verified checkpoints are committed before releasing snapshots and compressed bytes.

Pause, worker death, transfer/authentication failures, and staging Waiting preserve
the exact unfinished copy namespace. Resume re-hashes every selected source in a
verified group and validates the whole remote archive before reusing it. Size-only
providers require a download and SHA-256 check on the selected staging disk; this
download respects the staging budget, with the established whole oversized-file
exception. Actual download quota failures enter actionable Waiting as well. Prepared
records survive upload-before-checkpoint crashes but do not count as verified work
or authorize reuse; this slice retries their unfinished content from source snapshots.
Prepared-upload reconciliation and source/remote-change reconciliation follow in #51.

Controls are checked during snapshots, compression callbacks, verification,
checkpoint publication, finalization, and owned CLI execution. Paused work is not
scheduler-ready; Cancel is terminal and a subsequent request gets a new namespace.
Interrupted attempts never authorize retention. See
[archive recovery acceptance coverage](native/tests/archiverecovery.md).

The UI checks the CLI connection asynchronously with
`filesystem info /my-files --json` at startup, when the window becomes active,
and every 30 seconds.
When disconnected, **Sign in to Proton** launches the configured CLI's `auth login`
through `xdg-terminal-exec`. Credentials remain managed by Proton's CLI.
Connection failures also expose a retry button and the CLI error in the sign-in
button's tooltip.

The Proton Drive provider uses the official CLI for:

- uploads with a parent folder, file replacement, and folder merge for recursive transfers;
- downloads with remove conflict handling;
- discovery through `filesystem list`;
- cleanup through per-item `trash` followed by `delete`.

OmaCustos never calls `empty-trash`. Remote content size is verified after upload and
download using `size` or `activeRevision.claimedSize`, not encrypted storage size.
Each payload is copied into a private temporary folder and hashed while copying.
The engine's private `stagePayload` operation opens the source, creates the mapped
snapshot, copies and hashes it, makes it read-only, and removes partial snapshots
on failure. It returns only the staged path, size, and SHA-256; the orchestrator
owns the temporary directories and controls their lifetime through upload/retry.
The resulting read-only staged file is uploaded under the requested remote
basename, so source edits or pathname replacement cannot invalidate the recorded
SHA-256. Worker backups use `backupBatches`: at most 1000000000 payload bytes and
1000 files per prepared batch by default. A larger file is staged alone after a
free-space check, including a 16 MiB metadata reserve. Actual writes and flushes
can still fail under a user quota; those failures produce a waiting state with
verified work preserved. Source-read failures instead continue other files and
finalize an Incomplete manifest containing only verified entries.

`BackupStaging` selects `~/.local/share/omacustos/staging` outside sources, or a
private `/var/tmp/omacustos-<uid>/staging` fallback. Explicit `staging_directory`
and positive `staging_budget_bytes` settings are persisted/exported per set.
Locations resolve existing ancestors and reject source overlap, symlinks at the
chosen directory, RAM-backed/read-only/unusable filesystems. There is no `/tmp`
fallback. Private workspaces have a durable ownership marker and lifetime lock.
Startup recovery considers recorded staging roots even when all work is paused;
only same-user marked, unlocked children outside selected sources are removed.
Existing user-chosen base-directory permissions are preserved.

Folder sources upload each mapped prepared tree with CLI folder `merge`. A retry
uses the same read-only bytes. File-only sources/providers without recursive
upload transfer individual snapshots inside the batch. Staging remains through
verification and checkpoint publication and is released before preparing the
next batch. Production final manifests also live inside owned disk staging, so
crash recovery covers finalization payloads.

A provider SHA-256 field is used when the CLI exposes one. Existing remote payloads
are reused only after re-hashing current local content and verifying remote content.
When metadata lacks SHA-256, continuation downloads and hashes the remote payload
instead of trusting same-sized content. This also reconciles transfers completed
before verified checkpoint publication. A failed merge cannot use size-only
metadata to certify old unmodified remote content: checksum evidence is required.
New copy namespaces skip pre-upload payload lookup. Each batch obtains one fresh
content-metadata listing per payload directory, falling back to individual
inspection for missing, malformed, or ambiguous entries. Unsupported/failed or
storage-only listings disable further bulk attempts for that pass. Only verified
payloads enter the manifest or verified progress counts; manifest verification still
applies. Directory caches have batch lifetime, and payloads are released only after
the verified batch commit. Engine callers
reusing a namespace retain checksum-based reuse checks and immediate individual
post-upload verification by default.
Within a backup operation, each payload parent directory is ensured once, and the
Proton provider also caches its existing/created ancestors. A failed upload
invalidates the provider's ancestor cache and the engine's parent result, then
rechecks the full parent chain before one retry of the same staged bytes. Caches
are cleared at operation entry and exit, including early failures.

Copy verification can use one folder listing per payload directory when it supplies
valid content sizes (`size` or `activeRevision.claimedSize`) and optional SHA-256.
Encrypted `totalStorageSize` is never accepted for verification. Missing, malformed,
or ambiguous listed entries fall back to individual metadata inspection. An
unsupported, failed, or storage-only listing disables further bulk attempts for
that verification. Directory metadata is cached only for the current copy check;
each subsequent selection or retention discovery obtains fresh metadata.
Backup finalization and catalog checks share the same operation-local metadata cache.
Manifest validation uses a failed-path set rather than scanning the failed list for
each verified entry, keeping membership checks linear in the total item count.

`QProcessRunner` uses a five-minute total runtime limit for metadata and other
commands, and a separate 24-hour limit for `filesystem upload` and
`filesystem download`, including manifest transfers. A positive integer
`OMACUSTOS_TRANSFER_TIMEOUT_SECONDS` overrides the transfer limit (maximum
2147483 seconds); invalid values fall back to the default. Worker and GUI
environments are independent, so configuring only the service changes backups,
while restores use the app's environment.

These are wall-clock limits, not inactivity detection: CLI output is not treated
as proof of byte progress and cannot extend the deadline. Process startup is
bounded to 30 seconds or the operation's limit, whichever is shorter. Startup,
timeout, abnormal exit, and nonzero-exit diagnostics remain distinct and preserve
CLI stderr. Timed-out commands are killed and return a failure even if partial
remote content exists. The normal manifest verification and success-only
retention gates still apply. Tests inject millisecond timeout policies and use
a local subprocess fixture for progressing, silent, and failing commands.

### Transfer Timeout Configuration

To change the transfer limit, set `OMACUSTOS_TRANSFER_TIMEOUT_SECONDS` to a positive
whole number of seconds in the app's environment. For manual and scheduled backups,
set it in the worker service using `systemctl --user edit omacustos.service`:

```ini
[Service]
Environment=OMACUSTOS_TRANSFER_TIMEOUT_SECONDS=172800
```

Run `systemctl --user daemon-reload` afterward; the setting applies to the next
worker. For restores, launch the app with the variable, for example
`OMACUSTOS_TRANSFER_TIMEOUT_SECONDS=172800 omacustos`. This example allows 48 hours
per transfer. Empty, invalid, nonpositive, or values above 2147483 seconds use the
24-hour default.

### Browser Links And Copy Management

Recent-backup browser links open the copy recorded for that run in
the signed-in Proton Drive web app. Browsing a recorded copy uses its locally saved
path directly, without downloading its manifest or acquiring worker/run-state locks.
The worker prepares and caches the private browser URL for manifest-verified copies
before publishing their final result; a link-lookup failure never changes the backup
result. The app also prefetches uncached recorded-copy URLs in the background at
startup and as recent backups change, without opening the browser or showing errors.
Reconnecting to Proton retries previously failed prefetches.
Cached links are emitted immediately without a CLI request, including after restart.
An uncached click joins prefetch for that exact copy or starts its own lookup without
waiting for another copy's prefetch.

Link resolution uses the folder's node ID and a share ID obtained through read-only
CLI metadata requests. For the usual `/my-files` layout, it inspects the target and
the top-level folder directly (at most two commands), rather than walking every
parent. An ancestor fallback handles metadata that exposes the share only on a
nearer parent. No public sharing links are created. The cache retains at most 200
URLs and is merged atomically under a separate lock shared by the GUI and worker.

Runs persist the exact copy folder as `remote_copy_path`. For older run records,
OmaCustos identifies the newest matching manifest inside that backup's folder and
remembers its path through a fresh coordinated run-store update. This legacy fallback still
downloads manifests. Browsing and manual deletion use the same recorded copy.
Manual deletion requires confirmation of the exact path, rechecks its OmaCustos
manifest and identity, and moves that copy to Proton Drive Trash. Older copies
and the backup set remain; the deleted entry disappears from Recent backups.
Config-specific and shared queue-worker locks prevent deletion during a backup.
Discovery and deletion release queue coordination between durable mutations, so
manual requests can be saved during remote calls. Copy-pointer and deletion
updates merge fresh state; deleting an old copy preserves a new manual request
for the same backup instead of replacing its pending status with `copy_deleted`.

## Backup Progress And Remaining Time

The engine reports planned file sizes, processed files and bytes, and a finalizing
phase, along with verified file/byte counts, failed-item counts, current file and
size, and reading/checking/uploading/verifying phases. The worker persists these
progress samples in run state, throttled to approximately once per second except
for initial progress, completion of the planned file attempts, and finalization.
File and phase changes coalesce into the latest sample. A serialized progress
writer also publishes pending samples during synchronous long-running transfers,
and is joined before durable success/failure state is saved.
Processed counts include failed attempts; they describe work done, not verified
backup contents. Manifest verification remains the authority for successful files.

Modern worker attempts show their current phase and verified byte/file counts
instead of estimating throughput or ETA from the CLI's final JSON summary.
Historical elapsed/count fields and the old estimator remain readable for record
compatibility, but do not drive the new continuation UI.

The UI polls once per second while a backup is running and every five seconds
otherwise. Unchanged state contents skip JSON parsing and model notifications;
phase/progress presentation updates independently of the recent-backup model. Ordering, summaries,
and transfer display data are cached once per refresh. Detailed issue lists are
converted only when requested, including while the details dialog is open.
Manifest upload/verification show **Finalizing backup…**. Success is durably
published before retention runs; a concurrent pause/cancel cannot authorize
older-copy deletion.

The work-progress bar measures processed file attempts rather than upload bytes
or verified contents. The CLI has no documented live byte-progress feed; the
current path, planned size, and phase remain visible during single-file transfers.
Folder backups show separate preparation and batch-folder upload phases with an
indeterminate work-progress bar. Staging a file does not count as uploading it;
processed counts advance only after the folder command finishes, and verification
counts advance after remote verification and durable checkpoint publication.

Run state also stores a structured `result` with verified payload counts, issues,
and whether the remote manifest passed verification. Only a verified manifest with
some verified entries can produce an **Incomplete** result. Source failures with
no verified entries produce **Failed**. Transfer, verification, manifest, and
checkpoint interruptions instead keep the copy unfinished in **Retrying** or an
actionable **Waiting** / **Sign-in required** state. Only **Successful** runs
trigger retention cleanup. Older run records without these fields remain readable
and do not display invented verified-file counts.

Payload batching does not bound all metadata. Selection paths, collision maps,
manifest entries, continuation identities, and provider listings still scale with
file count; provider listings can include more than the current batch. Batch
journals avoid quadratic full-checkpoint rewrites, and verification caches are
discarded between batches. The opt-in [large-backup resource benchmark](native/tests/largebackup-benchmark.md)
records selection/checkpoint, execution, and final-manifest RSS separately for
877172 files / 95323650798 selected bytes. The high-water mark is measured for the
process, not an invented constant-memory guarantee or a live CLI throughput estimate.

## Retention And Cleanup

Retention considers only positively identified OmaCustos copies for the
correct computer and backup. Successful verified copies are retained according to
the backup's limit. Failed or incomplete copies cannot cause an older successful
copy to be removed.
Post-backup discovery lists direct copy directories under that computer's backup
folder and verifies their manifests and payload metadata. It does not recursively
scan other computers, backups, or payload directories. Full-root discovery remains
available for catalog discovery.

Before the first cleanup, the exact remote paths and a proposal identifier are
persisted and shown in the UI. Confirmation compares the displayed proposal
with current disk state; a changed proposal is refreshed and must be reviewed
again. No deletion occurs until the proposal is durably confirmed. Later
cleanups use that saved decision, but persist each new exact scope before the
first trash or permanent-delete operation. A failed state write stops cleanup
and reports a retryable error.

The cleanup module coordinates all updates with a shared file lock and reloads
fresh state before writing. The lock remains held through deletion and phase
persistence, so stale UI or worker snapshots cannot overwrite another set's
progress. An unfinished authorized scope is never replaced by newly discovered
targets. Retries resume its persisted phases, including when a remote operation
succeeded but its phase write failed. Targets must be strictly below the backup
set's configured root; the root itself cannot be deleted.

## Restore Safety

CLI downloads use a private staging directory so their remote-basename conflict
handling cannot remove destination files or unrelated files and folders. Restore
payloads stay in private staging until size and SHA-256 verification passes.
Verified bytes replace the destination atomically with `QSaveFile`, with direct
write fallback disabled. Hashing and writing the atomic replacement share one
payload read, and commit occurs only after size/checksum verification. The Proton
provider moves completed downloads into unused private staging paths on the same
filesystem instead of making an additional copy; existing provider destinations
retain atomic replacement. Download, verification, or placement failures preserve
existing destination content, and staging is cleaned up on every return path.
Destination and parent symlink checks are repeated after the transfer before
writing the replacement and again immediately before committing it.

Opening Restore lists only direct copy folders for the selected backup, using
the recorded run's copy parent when available and the configured computer/backup
folder otherwise. Listing runs in a background task; it does not traverse payload
directories, download manifests, or inspect files. No copy is selected automatically.
Selecting a copy downloads its manifest and verifies only that copy's files in
the background. Loading indicators distinguish listing from verification; conflicting
restore actions are disabled while either task runs. Each selection rechecks remote
files, and manifest backup/copy identity must match the selection. Full verified
catalog discovery remains available internally for retention.

Restores are limited to manifest entries that passed verification. Destination
traversal and symbolic-link escapes are rejected. The UI requires users to tick
files and specify a destination folder before enabling its single **Start
restore** action. Restore metadata is not added to the local backup queue.
The controller emits completion only after every selected file restores
successfully. Starting replaces the selection screen with `RestoreProgressPanel`,
using the same `TransferProgress` component as backup. Terminal summaries remain
visible until Done or Back to selection. Done clears selection/destination after
success; failure and stop preserve them for an explicit full-selection retry.

Restore transfers retain their backup/copy identity and selected payload snapshot
along with immutable display context, destination and download cost. Navigation
and controller context changes are locked during a transfer. A controller-owned
serial executor keeps its provider calls serialized. Superseded browse results,
including errors, cannot publish into the current context.
The controller submits the complete captured selection, destination, and copy path
to `BackupEngine::restoreFiles` once per operation. The engine owns execution and
reports successfully placed files and the final partial/success outcome; the
controller bounds GUI progress notifications. Single-file engine callers delegate
to this same operation, sharing staging, verification, and atomic placement.
An operation-owned control token supports in-session cooperative Pause/Resume at
safe engine checkpoints. A condition variable retains the live workspace while
paused; Stop wakes it and unwinds temporary work, retaining committed destination
files. Stop is checked during atomic replacement copying and before commit. Active
Proton downloads use a dedicated restore runner with a stop predicate; unrelated
copy management and folder lookup use a separate runner. Archive verification and
extraction finish their current phase before acknowledging controls. Window close
requests Stop and waits for completion; controller destruction also wakes paused
work before waiting. No durable restore continuation is written. Fail-fast restore
results include `{path, phase, reason}` issues, shown as wrapped plain text in the
restore issues dialog; intentional stopping is a distinct outcome, not an error.
Superseded verification is cancelled between provider calls, including before a
queued task starts and after manifest download. An already-running provider call
finishes under its normal timeout before the latest browse request proceeds.

The controller retains bounded last-good display snapshots for 20 backup contexts.
Returning to a context invalidates restore eligibility until current verification;
copy identity, destination validation, and checksums remain authoritative. The
interface retains destination, valid file ticks, and scroll position per backup.
File-path display lists are cached, and selection remapping builds one path index
per refresh rather than repeatedly reconstructing or searching the 10,000-file list.
Individual checkbox toggles mutate owned selection arrays and update an index-to-
position lookup. Deselection swaps with the last item rather than shifting the
remaining selection; selection order is not significant. Property notifications
and visible checkbox state still update without rebuilding the full selection.
Selection arrays are read-only properties of the owner; bulk replacement uses
`Main.setRestoreSelection(indexes)` rather than separate index/path assignments.
Both bulk changes and toggles publish array notifications after paths, lookup,
counts, folder states, and customization flags agree. Selection snapshots include
the entry list so returning to another backup can rebuild its folder state before
the controller publishes refreshed entries. Cached selection does not change the
controller's restore-eligibility gate.

Source preview scans run asynchronously against captured source/exclusion lists.
Selection/input changes invalidate obsolete results, and repeated requests coalesce
to the latest pending scan. Loading feedback distinguishes the previous preview
from the pending result. Superseded scans and controller shutdown cancel traversal
between source paths/directory entries; cancelled scans discard partial results.
Local-state refresh failures retain last-good run/cleanup
data and expose a persistent refresh error.

See [Responsiveness measurements](native/tests/responsiveness.md) for the benchmark
method, separate workload results, and remaining hardware/presentation validation.

## Desktop Theme

The UI reads Omarchy's `colors.toml` from `$XDG_STATE_HOME/omarchy/current/theme`
(default `~/.local/state`) or the legacy `$XDG_CONFIG_HOME/omarchy/current/theme`
(default `~/.config`). File and directory watches pick up palette edits and
whole-directory replacements during theme switches. Missing or invalid palettes
fall back to the system Qt palette. UI surfaces, text, controls, selections, and
disabled colors share these live palette roles.

## Services And Packaging

The package installs these user units:

```text
/usr/lib/systemd/user/omacustos.service
/usr/lib/systemd/user/omacustos.timer
```

By default, the worker and its Proton CLI subprocesses use normal system
scheduling with no CPU quota. Packaged and generated base service units do not
set CPU or I/O resource limits. **Use system defaults** is checked unless an
explicit resource preset has been saved. Previously selected presets remain
selected after an upgrade.

The **Resource usage (all backups)** control offers five opt-in global presets:
very low (10%, nice 19), low (25%, nice 15), medium (50%, nice 10), high (100%,
nice 5), and very high (200%, nice 0). Very low and low use idle I/O scheduling;
medium, high, and very high use best-effort I/O with priorities 7, 5, and 4.

Saving a changed preset writes a managed drop-in at
`$XDG_CONFIG_HOME/systemd/user/omacustos.service.d/50-omacustos-resources.conf`
(default `~/.config/systemd/user/...`) and asynchronously runs
`systemctl --user daemon-reload`. Reload failures restore the previous file and
report an error. No root privileges are required. The drop-in is also the
persistent preset store; backup-set import/export does not change it.
Returning to **Use system defaults** clears `CPUQuota`, sets `Nice=0`, and
restores normal I/O scheduling (`IOSchedulingClass=none`, priority 0). These
explicit resets also override limits in older base service units. The default
selection is represented internally by preset index -1; indices 0–4 remain the
five optional presets.
Both manual and scheduled backups start the same service, so the limits apply
to the worker and its CLI subprocesses from the next worker start. A running
backup is not restarted. Quotas are measured against one CPU core, so 200%
permits up to two cores. Priorities never exceed normal (`nice=0`).

The package installs the timer without enabling it. After a successful backup
configuration save or import with any enabled schedule, the UI asynchronously
runs `systemctl --user daemon-reload`, then
`systemctl --user enable --now omacustos.timer`. Activation waits for any
resource-preset update to finish, so an immediately due backup starts with the
saved limits. Invalid or failed saves,
preview, and manual-only configurations do not enable the timer.

The scheduler checks the timer's `LoadState`, `ActiveState`, and `UnitFileState`
at startup, after activation, on window focus, and every 30 seconds. It reports
active, paused, session-only, and unavailable states in the UI. Activation errors
leave the saved schedules intact and provide an in-app retry. Opening the app
does not automatically resume an externally paused timer. Disabling a backup's
schedule stops future scheduled runs for that set; the shared timer may remain
active to process queued work and retries.

The full project and desktop launcher name is **OmaCustos for Proton Drive**;
the interface uses **OmaCustos**. The package is `omacustos-git`, the executables
are `omacustos`, `omacustos-worker`, and `omacustos-install`, and the user units
are `omacustos.service` and `omacustos.timer`. The QML module is `OmaCustos`.
`OMACUSTOS_PROTON_BIN` overrides the default Proton CLI executable when creating
a configuration; saved configurations retain their `proton_binary` setting.

## Development

Build and test the native app from the repository root:

```bash
cmake -S native -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The main binaries are `build/omacustos` and `build/omacustos-worker`. The Arch package
recipe is in `pkgbuild/`.

See [Responsiveness measurements](native/tests/responsiveness.md) for the opt-in
on-screen benchmark, before/after results, and outstanding target-hardware checks.
