# Incremental repository contract (v4)

Status: normative design for #56, not enabled in the application. Changed-file
incremental backups replace full uploads only after the release gates below pass.
The pure `IncrementalManifest::validate` boundary implements the manifest schema;
it performs no network I/O and does not authorize publication, restore, or deletion.
Existing v1/v2/v3 readers, exact v3 membership checks, and full uploads are unchanged.

## Scope and trust

Target documents and source code. Changed files are saved in full, in bounded
tar.gz archives (gzip level 3). Capture is file-by-file, not filesystem-wide or
application-consistent. No block deduplication, cross-set sharing, compaction,
historical archive migration, or live database/VM consistency is promised.

Trust the authenticated account, TLS/CLI transport, and the owning implementation
to report verification honestly. SHA-256 protects against accidental corruption,
not a malicious account writer replacing both data and manifests. A receipt is
durable evidence of an observation, not a signature or a fresh remote check.
Untrusted downloaded JSON, paths, and archives always require validation. External
account tools can bypass coordination: concurrent manual edits/deletion are outside
the safety model and must be warned against. Integrity checks detect such damage;
they cannot make an account administrator obey the protocol.

## Stable identity and namespace

Generate cryptographically random UUIDv4 identifiers: one `computer_id` persisted
outside editable hostnames, one `set_id` at set creation, and fresh object, snapshot,
and operation IDs. Lowercase canonical UUID text is mandatory. Export/import on
another installation never grants write ownership. Cloning a computer identity
requires explicit recovery, not automatic scheduling. Renaming computer/set labels
does not move storage or alter identity. Losing local identity makes discovery
read-only until takeover. Preserve the original computer ID across ownership changes.

```text
<chosen-remote-root>/sets/<set_id>/
  repository.json
  objects/<object_id>.tar.gz
  snapshots/<snapshot_id>/manifest.json
  operations/<operation_id>/...       # durable protection and recovery records
  maintenance/...                     # integrity coverage and deletion intents
```

`repository.json` is a version-1 control record: `application: "omacustos"`,
`set_id`, immutable `origin_computer_id`, current `owner_computer_id`, positive
`ownership_epoch`, and human-readable `set_name`/`computer_name`. Provision it with
exclusive create, verify by readback, and never adopt an existing conflicting ID.
Control updates are conditional on its revision and current fenced ownership.
Display labels are advisory, never scope or authorization. The root is chosen
explicitly during remote-only recovery; discover repositories below its direct
`sets` child, not by guessing hostnames. Legacy discovery remains separate.

All mutation paths must be constructed below the exact validated set root from
typed IDs; manifests cannot supply an absolute remote path. Reject root aliases,
parent traversal, separators in IDs, mismatched set/snapshot selections, and
provider node identities whose resolved parent is outside that root. A set root,
`objects` root, or `snapshots` root is never a deletion target.

## Manifest schema

Examples: [baseline](../native/tests/fixtures/incremental/baseline.json) and
[later shared subset](../native/tests/fixtures/incremental/subset.json).
These are metadata-only illustrative fixtures, not downloadable archives; hashes
and compressed size are placeholders. The latter deletes `b` and renames `a`, but
embeds the complete original archive inventory. Neither requires the other manifest.

Every object below has **exactly** the listed keys (no silent extensions). Arrays
must have the indicated types. Reject unsupported versions rather than falling
back to v3. Numbers are JSON integers in 0..2^53-1 unless narrowed below; hashes
are 64 lowercase hex SHA-256 characters. Timestamps use valid UTC
`YYYY-MM-DDTHH:mm:ss.sssZ`. JSON object keys must be unique at the producer; Qt's
JSON parser resolves duplicate keys before schema validation, so do not use raw
JSON byte equivalence or duplicate-key interpretation as an authorization signal.

| Object | Required fields and constraints |
| --- | --- |
| Manifest | `version: 4`, `application: "omacustos"`, `set_id`, `origin_computer_id`, `owner_computer_id`, `ownership_epoch` (positive), `snapshot_id`, `created_at`, `status: "complete"`, `archives` array, `entries` array |
| Archive | `id` (object UUID), `remote` (exactly `objects/<id>.tar.gz`), `size` (positive compressed bytes), `sha256` (compressed bytes), `members` (nonempty complete inventory), `original_verification` |
| Physical member | `path` (safe relative tar name), `size` (uncompressed bytes), `sha256` (file bytes) |
| Original verification | `method` (`download-sha256` or `remote-sha256`), `verified_at`, `sha256` (must equal archive digest); embedded in the archive descriptor, binding its ID, size, complete inventory and digest |
| Logical entry | `restore` (safe relative destination), `archive` (object UUID), `member` (physical path), `size`, `sha256` (both exactly match physical member), `mode` (integer 0..511, POSIX permission bits), `mtime_ns` (canonical unsigned decimal **string**, 0..9223372036854775807) |

All IDs use the UUIDv4 representation above. Set/snapshot IDs must match the
selected remote location; original computer identity must match the repository
anchor. Old snapshots may have an earlier owner/epoch; this does not prevent restore.
The pure validator checks schema and selected set/snapshot, not the remote anchor.
Archive IDs and physical names per archive are unique. Every archive must have at
least one logical reference, but **not every physical member need be referenced**.
Multiple destinations may reference the same physical member (identical files).
Destinations are unique; both physical and destination sets reject file/parent-file
conflicts. Empty entries plus empty archives represent a complete empty saved state.

Relative paths reject empty components, `.`, `..`, leading/trailing `/`, backslash,
NUL, ASCII controls and DEL. Names are case-sensitive and not Unicode-normalized.
On filesystems with incompatible name equivalence, fail on collisions before
placement, never merge/overwrite silently. Sources and local configuration are not
required in the manifest. A complete manifest is the whole logical saved state,
never a delta, ancestor pointer, or list of just changed files. Source failure
prevents a v4 complete publication; preserve resumable work and older snapshots,
report the failure, and do not run retention. Legacy incomplete copies stay readable.

### Restore metadata and validation

Support regular file bytes, relative destination, POSIX mode bits 0777 and mtime
in nanoseconds since Unix epoch (nonnegative signed-64-bit range). Compare all of
these for unchanged-state detection; a metadata-only change creates a new manifest
but may reuse content. Hash every selected local file; size/mtime is no substitute.
Reuse from the immediately previous complete snapshot by size/SHA-256, including
renames; no search through arbitrary historical snapshots yet. Deterministically
choose the lowest object-ID/member tuple when several matches exist.

Do not preserve owners/groups, ACLs, xattrs, sparse allocation, birth/ctime,
directory metadata or empty directories, symlinks, special files, hardlink
identity, setuid/setgid/sticky bits. Explain these limits in the restore UI. Files
are staged privately, then mode/mtime applied to the verified temporary file before
atomic placement. Unsupported metadata or precision loss is reported, not silently
claimed as exact restoration. Recheck destination symlinks before commit; no direct
write fallback. Already committed files retain existing partial-restore semantics.

For every downloaded archive, check compressed size/hash, complete gzip stream,
CRC/trailer and tar termination. Validate **all** physical members, including
unselected ones, against the full inventory (names, size and SHA-256). Reject extra,
duplicate, missing, unsafe, nonregular or linked tar members. Do not materialize
unselected members. Tar permission/time fields are not restore authority: use
logical metadata, so reused content can have different mode/mtime. Never adapt the
v3 extractor by merely allowing unexpected members. A future v4 restore adapter
must explicitly pass the physical inventory and logical selection separately.

## Ownership and enforced coordination

Serialize backup, full integrity maintenance, retention, and recovery mutations
per set. Read-only discovery/restore need no write ownership. Local process/file
locks remain useful but are insufficient across computers or cloned configurations.

**Required provider capability:** a linearizable per-set mutation gate that grants
an exclusive session with a monotonically increasing fencing token tied to the
ownership epoch. Every object/control upload, manifest create, trash and permanent
delete must be rejected server-side for an expired/revoked session or stale token.
An exclusive-create immutable name and atomic complete-file visibility are also
required. An advisory lock file, check-then-write, clock-based timeout, or CAS of
only `repository.json` while payload mutations remain unfenced does not qualify.

The current Proton CLI adapter has no demonstrated such gate. Therefore **v4
mutations are unsupported and must fail closed** until a provider implementation
proves these capabilities or an enforceable serialized mutation service supplies
them. Do not turn this contract into a best-effort lease on the current CLI. The
read-only schema work in #56 does not pretend to implement a remote lock.

Takeover is a separate user-confirmed recovery action: show exact root/current
owner/epoch; atomically revoke and fence all old sessions, increment the epoch,
assign the new computer ID, then acquire a fresh exclusive session. Reconcile all
old operations/intents before writes or cleanup resume. Old journals cannot resume
under a new epoch without explicit adoption and fresh verification. If revocation
or current ownership cannot be proven, remain read-only, never steal a lock based
on age. A crashed session may be revoked by the same owner through this same gate;
an old paused process must receive a hard stale-token failure on its next mutation.

## Verification evidence and health policy

1. Before upload, journal an immutable object descriptor and inventory to synced,
   atomic local state, and persist a verified remote operation protection record.
2. Upload exclusively to a fresh ID. Fully verify compressed size and SHA-256
   against private staged bytes. A trustworthy provider checksum must describe
   the current **plaintext content revision**, not encrypted storage size, ETag,
   upload-request echo, user metadata, or a different revision. Otherwise download
   and hash; quota/staging shortage enters Waiting, not size-only success.
3. Only then persist `original_verification` locally and remotely in that operation
   record, read/hash it back, sync local verified checkpoint, then release staging.
   Copy the descriptor and original receipt unchanged into every referencing
   manifest. Thus loss of local state does not lose the evidence. Conflicting
   descriptors/receipts for one immutable ID are corruption: stop and investigate.
4. On every daily reuse/check, require that evidence and matching identity; fetch
   fresh existence and plaintext size plus trustworthy SHA-256 if available.
   Do not refresh `verified_at` based on size. Missing evidence requires a new full
   verification and durable receipt before reuse. Authentication/network/ambiguous
   listing errors are unknown health, not absence: stop/retry, no cleanup.

Without trustworthy remote hashes, daily checks can miss same-sized corruption
until a full check or restore. Display “Existence/size checked; full integrity last
checked <time>; next due <time>”, never “all bytes verified today”. Local hashing
does not prove remote health. Restore always hashes downloaded bytes independently.
Confirmed damage creates a fresh replacement object from current local bytes when
possible, never overwrites an ID. Report every historical snapshot affected; a new
successful backup does not repair old manifests or authorize deleting damaged history.

### Scheduled full integrity maintenance

Begin a full cycle at least every 30 days since the last completed cycle, and on
explicit Check integrity. Loss of the remote coverage ledger makes it due now.
Coverage is every live shared object reachable from **all retained snapshots and
protected operations**, not just the newest copy. Validate every retained manifest
first. Each object gets current revision size/hash verification; download/hash
when trustworthy SHA-256 is unavailable, then validate its full physical inventory
as for restore. Even with a remote checksum, physical structure must have been
validated at original creation; a changed revision requires downloaded validation.

Capture a sorted `(object ID, size, digest, inventory)` worklist under coordination.
Use the existing configured staging disk/budget and one archive at a time, with
the existing explicitly reported single-oversized-file exception. Default run
budget: 1 hour or 10 GiB downloaded, stopping between archives (one archive can
exceed the soft budget). Resume next daily worker invocation before cleanup; show
coverage count/bytes, last completion, remaining work and overdue status. Never
mark a cycle complete merely because its budget expired. Do not starve maintenance
behind repeated backups: once due, advance its slice before the next backup.

After each verified object, atomically persist remote coverage (cycle ID, expected
descriptor, checked revision, method and timestamp) and sync the local cursor.
Crash resumes the first uncommitted object; an uncertain commit is rechecked.
Pause preserves the worklist; cancellation/lost ledger restarts coverage, not a
false success. Before completion, reconcile reachability: add newly live objects
unless full verification since cycle start already covers them, drop only proven
unreachable ones. Bind evidence to revisions; unknown/changing revisions require
rechecking. Completion records its exact coverage and timestamp remotely. Maintenance
failures preserve history, expose affected snapshots, and block automatic GC until
resolved. These are logical budgets, not a claim of bounded network-call duration.

## Manifest-last publication and interruption

An operation protection record (version 1) contains set/operation/snapshot IDs,
owner/epoch, captured selection, all prepared and reused object descriptors,
verification states, and phase. Persist and verify it under the fenced session
**before** uploading or claiming any object. Extend it durably before each new
object; local journals alone never protect remote reachability. Never overwrite
published objects/manifests. Unknown upload results are reconciled by exact ID and
hash; conflicting bytes cause a new ID, never replacement.

Ordering: prepared local journal → remote protection → upload → full verification
→ remote receipt → synced verified local checkpoint → release staging → daily
reuse checks → validate complete manifest → exclusive atomic manifest upload last
→ download/hash/schema/location verification → durable run success → operation
completion → retention eligibility. Manifest bytes must be assembled before upload;
the provider must not expose a partial file at its final name. A successful
unchanged check records last successful check remotely in the operation history
without publishing a snapshot or consuming retention; retain last saved-change time.

Discovery lists only direct snapshot IDs, then reads `manifest.json`. Missing file
means unpublished; malformed file is visible as damaged, never restorable or cleanup
authority. A valid complete manifest at its final path is the commit point; a lost
upload response does not make it unpublished. Remote-only discovery needs no run
history. Invalid or unknown-version snapshots block cleanup, rather than being
silently ignored. The app must not advertise success until readback succeeds.

| Interruption | Recovery under fenced coordination |
| --- | --- |
| Before remote protection | No upload permitted; discard/rebuild local preparation |
| After protection, before/within upload | Protect named objects; inspect exact names; retry missing objects, fully hash existing objects before accepting them |
| After upload, before receipt/checkpoint | Reverify; prepared state is not verification evidence |
| After receipt, before staging release | Validate receipt/identity and current reuse health; checkpoint before release |
| Before manifest | Resume same snapshot/operation; rescan current sources, reconcile refs, and update protection before changing objects |
| Manifest upload or readback outcome unknown | Read exact path; identical valid bytes mean committed; absent means retry; conflicting/invalid bytes mean quarantine and stop |
| After manifest, before local success | Recover committed snapshot and success without uploading a duplicate; cleanup remains gated on durable success |
| Pause/cancel/owner loss | Pause retains protection indefinitely; cancel records terminal abandonment under gate; never infer abandonment from time alone |

Protection is retired only after a verified committed snapshot takes over its
references, or explicit abandonment by the current owner after fencing old sessions.
Lost/unreadable operation state blocks GC. Reinstall recovery enumerates remote
operations before resuming; it never treats local journal absence as abandonment.

## Retention, deletion authorization, and recovery

Keep configurable newest-N successful saved states across positively identified
legacy copies and v4 snapshots, sorted by creation time then snapshot/copy identity.
Unchanged checks consume no slot. Preserve incomplete/damaged/unknown states unless
separately and explicitly authorized for removal; they never evict good history.
Upload the fresh baseline without referencing legacy archives. Check capacity and
warn about coexistence with old full history; insufficient capacity stops the run,
never deletes retained history to make room.

Only a durably successful, verified publication can initiate automatic retention.
Hold the fenced gate throughout planning and mutations. Read **all** manifests,
protection records, unresolved maintenance/deletion intents, and repository anchor
freshly. Any unreadable, ambiguous, malformed, foreign or unsupported record stops
GC. Do not use a possibly incomplete listing: provider must guarantee complete
enumeration or a revision-consistent inventory. No age-only orphan collection.

Persist/readback a deletion intent (version 1): proposal UUID, set/owner/epoch,
exact snapshot paths and manifest hashes, legacy copy paths/identities, exact
object IDs/paths/digests/sizes, reachability generation, confirmation and per-target
phases. First cleanup requires exact-scope user confirmation, matching current
proposal bytes; changed scope requires fresh confirmation. Later cleanup uses the
saved policy but still durably records each exact scope before mutations. Existing
local cleanup locks and run coordination remain required, not sufficient fencing.

Order: authorize intent → remove only approved snapshot manifests/copy folders →
persist each result → freshly recompute retained/protected reachability → trash
only whole unreferenced objects → persist phase → recheck reachability immediately
before permanent deletion → persist completion. No object may be trashed while a
retained or published-not-yet-removed snapshot, resumable operation or unresolved
intent protects it. The current intent's deletion candidates are not themselves
live references; other unresolved intents are protection until reconciled. New
writers cannot reuse objects already in a trash/delete intent; finish/reconcile it
first. Never delete partially live archives or guess compressed reclaim from member
sizes. Track logical live/obsolete bytes only as estimates.

Snapshot deletion first ensures a crash leaves extra objects, not dangling retained
references. After an uncertain trash/delete response, inspect exact identity and
trash state, then idempotently resume the persisted phase; never broaden targets
or replace an unfinished proposal. New epoch takeover revalidates every target and
requires renewed authorization. A transient failure stops the phase, retains intent
and reports retryable cleanup attention. Backup success remains success: “Backup
saved; cleanup needs attention”. No empty-trash command or deletion of other sets.
Legacy folder deletion retains existing containment/identity checks and never
authorizes deleting shared objects. Integrity errors cannot authorize retention.

## Implementation and release gates

#56 delivers schema tests and this contract, not remote mutation protocols. Future
implementations must prove the provider gate/visibility/listing guarantees first;
absence of those capabilities blocks the incremental rollout, not legacy backups.
Keep v4 outside `BackupManifest`/v3 restore until inventory-aware restore, catalog,
remote-only discovery and takeover are implemented together.

Test planner changes/renames/unchanged metadata, subset restore without ancestors,
legacy restore, exact tar validation, original evidence after local-state loss,
same-size corruption (daily limitation/full detection), missing objects, uncertain
network outcomes, quotas, pauses, stale-owner rejection, publication boundaries,
and deletion crash phases. Prove no retained/protected reference is deleted, including
unknown manifests and unfinished operations. Supply safe GC in the first usable
release. Benchmark baseline/unchanged/edit/full and single-file restore on documents
and code before choosing archive sizing (initial target approximately 1 GB).

Automated schema coverage runs via `incrementalmanifest-test` with real C++/Qt
validation, including shared subsets, location mismatches, unsafe references,
incompatible versions, missing/duplicate/conflicting inventory and receipt failures.
Protocol failure-injection tests belong with the later provider/writer/GC features;
schema acceptance is not evidence those remote protocols already work.

### Validation record (#56, 2026-10-20)

- `cmake -S native -B build` and `cmake --build build -j 4`: passed
  (existing optional Vulkan/QML-policy warnings only).
- `ctest --test-dir build --output-on-failure -j 4`: 24/25 targets passed;
  `workerrecovery-test` exceeded its existing 180-second CTest timeout, as already
  documented for #54 in `TECHNICAL.md`; no assertion failure was reported.
- Direct rerun `build/workerrecovery-test` with a 360-second external allowance:
  **139 passed, 0 failed**, 184.277 seconds. The CTest limit was not changed.
- After the final validator rebuild, targeted CTest for `incrementalmanifest`,
  `backupmanifest`, `backuprestorecontroller`, and `retention-discovery`: **4/4
  passed**. This covers the isolated schema and unchanged legacy boundaries.
- Pre-commit independent review of the staged scope against the original HEAD
  `7485d32b1220d37370164f93e0f9a1154b2320f9`: **Standards: no blockers;
  Spec (#56): no blockers**. Optional additional scalar-boundary test coverage
  was suggested; no documented-standard violation or requirement gap was found.
- `git diff --cached --check`: passed. No remote mutation or cloud test was run.
