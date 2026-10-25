# Reference-aware retention (#60)

> Historical component delivery record. Normal worker/UI integration now requires
> individual exact-proposal confirmation; see [the #62 report](incremental-default-rollout.md).

The advanced incremental executor supports opt-in cleanup. The normal worker is
still not switched to v4. All operations use the approved manual-offline,
single-owning-computer model, the shared state directory's non-expiring set UUID
lock, and fresh owner/epoch checks. This is not remote fencing.

## Use and authorization

Set `retention` to a positive newest-N count in the incremental JSON request.
Omit it (or use zero through the C++ API) to disable automatic cleanup planning.
Optionally set `legacy_root` to the exact old legacy set folder. It must be separate
from the shared repository; no recursive search or hostname guessing is used.
Legacy copies and snapshots share one creation-time/identity ordering. Incomplete
legacy states remain intact and consume no successful-history slot. Unreadable,
unsupported or ambiguous history blocks cleanup rather than being ignored.

An optional historical Proton folder that is absent contributes no copies, even
if one of its ancestors is also absent. Absence must be established from a complete,
name/UID/parent-validated ancestor listing; download errors, unreadable listings,
UID aliases and identity mismatches are never treated as absence. Required
repository inventories and exact deletion targets retain their strict checks.

Only a verified, durably completed **saved** publication can start a proposal.
Unchanged checks publish no snapshot and consume no slot. Failed uploads, quota
errors, and incomplete backups cannot start retention. No history is removed to
make upload space.

The saved result includes `cleanup_proposal`, its `cleanup_confirmation` SHA-256,
and `cleanup_needs_attention`. Review the entire proposal before copying that hash
into the request's `cleanup_confirmation`. Then run:

```sh
build/omacustos-incremental --request request.json \
  --acknowledge-single-writer --cleanup
```

`--cleanup` plans/retries cleanup without rerunning the writer. A fresh scope needs
a fresh confirmation; this implementation deliberately requires exact confirmation
for **every** proposal rather than granting an open-ended deletion policy. Once
authorization is persisted, retry needs no new confirmation for the same bytes.
Never pass a proposal from another set/root/owner. A pending proposal blocks all
writers, even before confirmation; finish/reconcile it before continuing backups.
Offline takeover refuses unfinished cleanup. Completed proposals survive handoff
as validated historical records, not reusable deletion authority.

Backup success is independent of cleanup success. If cleanup fails, the executor
keeps the saved snapshot and reports **`Backup saved; cleanup needs attention`**.
Do not retry by starting a different writer operation; use `--cleanup` with the
original saved request. No cloud commands were run during automated validation.

## Safety and durable ordering

1. Freshly validate every published manifest, operation identity, terminal and
   protection record, and existing deletion proposal. Any unfinished/resumable
   writer conservatively blocks **all** cleanup indefinitely, not just its known
   object references. Cancel explicitly or resume/finish it first; age is irrelevant.
2. Compute retained saved states and union their object/member references. Read
   complete provider inventories; providers without that capability fail closed.
   Unknown objects lacking protection metadata cannot be garbage-collected.
   Cancelled, uploaded-but-unverified objects require full downloaded hash and
   physical-inventory validation before becoming deletion candidates.
3. Persist/read back an immutable `maintenance/<proposal UUID>/proposal.json`.
   It binds the anchor, retention count, inventory generation digest, full expiring
   manifests, legacy paths/manifests, exact object descriptors, and stable provider
   node identities. Confirm its exact compact-JSON SHA-256 and persist/read back
   `authorized.json` before any destructive call.
4. Trash/delete only the approved snapshot **manifest files**, then separately the
   approved legacy copy folders. Persist each target's `*-trashed.json` and
   `*-deleted.json` receipt. Empty snapshot directories are harmless and remain.
5. Before **each** object trash and permanent delete, freshly recompute references
   from every remaining snapshot and revalidate terminal operations. A live or
   protected object cannot be removed. Before trashing existing candidate bytes,
   fully validate compressed hash/size and physical inventory. Never compact or
   delete a partially live archive.
6. Persist/read back `done.json` only after all target phase receipts exist.
   Unknown, malformed, missing or inconsistent maintenance phases stop mutations.

All receipts bind the proposal digest. Uncertain mutations repeat only the exact
persisted identity; no error-message substring is interpreted as proof of absence.
Local state loss does not lose the remote proposal, authorization or progress.
Completed proposals retain expired manifests solely to validate historical run
completion records. These archived manifests do not protect obsolete objects and
replaying their old operations cannot republish expired snapshots.

The local provider binds deletion to filesystem device/inode and validates live
and trash containment. The Proton adapter binds it to node UID, freshly enumerates
trash, and checks JSON per-node success rather than just process exit status.
Its destructive path support is deliberately limited to canonical name-based
`/my-files/...` paths: UID-shaped segments and other virtual roots are rejected.
It walks from `/my-files`, checking each listing's child UID/parent UID and the
resolved node's UID, parent UID and decrypted name. A lexically contained path is
not sufficient deletion authority. This also rejects unreadable ancestry and the
CLI's global UID alias resolution outside an apparent parent.
Permanent deletion uses `/trash/<name>`, never the original live path. Duplicate
trash names, unreadable trash entries, changed UIDs and unknown responses stop
cleanup for exact-node reconciliation; it never guesses or empties the trash.
External manual account changes and concurrently active cloned writers remain
outside the approved operational coordination model.

Before creating the remote proposal namespace, planning checkpoints the entire
proposal locally. If its initial upload fails, a fresh complete inventory may
ignore a **provably empty** unpublished maintenance directory; it has no deletion
authority. Any nonempty namespace missing its proposal still blocks mutations.
This recovery works after local journal loss and never infers absence from a
download error. A lost response after successful publication instead recovers the
exact existing proposal and still requires its exact authorization.

`estimated_live_member_bytes` and `estimated_obsolete_member_bytes` count unique
uncompressed physical-member bytes against retained logical references. They are
estimates of logical liveness, **not exact reclaimable compressed bytes**. Partially
live archives retain obsolete members, and a completely obsolete archive is the
smallest reclamation unit. Only currently present, identified archives are counted.

## Coverage

`incrementalrepository-test` covers mixed legacy/v4 history, empty snapshots,
overlapping member references, unchanged checks, source/upload quota failures,
unreadable retained manifests, exact confirmation and allowed-root limits,
resumed/paused writers, cancelled unverified uploads, newly live references,
deletion failures, replay of expired operations, and local journal loss. Data-driven
tests interrupt before mutation, after mutation but before receipt, and after receipt
for snapshot/legacy/object trash and deletion, plus authorization and completion.
The offline Proton fixture exercises UID-bound trash addressing, lost responses,
per-node failures with exit status zero, and ambiguous trash names.
Review regressions cover failed/lost initial proposal publication after local-state
loss, short/long global UID aliases, listed/resolved parent UID mismatches, and
offline handoff after completed retention.

### Missing historical-folder recovery (2026-10-24)

An absent optional Proton legacy namespace previously produced “Resolved Proton
node is not the selected child of its allowed parent” and blocked cleanup even
when current snapshots were healthy. Optional inventory now distinguishes proven
absence from identity/read failures; required inventories and deletion identities
still reject missing paths.

Regression coverage includes missing leaves/ancestors, existing empty folders,
unchanged exact-proposal confirmation, unreadable ancestry, UID aliases and parent
identity mismatches. A read-only capture of the affected repository failed before
the fix and its offline replay passed afterward without remote mutations. The full
CTest suite passed **41/41** after rebuilding all targets.

### Validation record (2026-10-20)

- `cmake --build build -j 4`: passed.
- Initial full `ctest --test-dir build --output-on-failure -j 4`: **28/28
  targets passed**, 178.22 seconds. Independent direct `workerrecovery-test`:
  **139 passed, 0 failed**, 172.70 seconds.
- After the review-driven recovery/containment fixes, targeted CTest covering
  incrementalrepository, protonprovider, protonclifixture, incrementalrestore and
  retention-discovery: **5/5 passed**. The repository target contains **76 passing
  cases**, including initialization/cleanup.
- Final full `ctest --test-dir build --output-on-failure -j 4` after all fixes:
  **28/28 targets passed**, 167.76 seconds, including worker recovery within its
  unchanged 180-second timeout.
- Pre-commit independent reviews against
  `036c03a034be9142a7430cf3dfd34a993eb67443`: **Standards: no blockers; Spec (#60):
  no remaining blockers**. The initial Spec review found failed-proposal-publication
  recovery and Proton UID-alias containment defects; both were fixed, regression
  tested, and independently re-reviewed before committing to `master`.
- `git diff --cached --check`: passed. No cloud mutation tests were performed.
