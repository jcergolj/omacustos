# Shared uploads and crash-safe publication (#58)

`IncrementalRepository` executes v4 plans through `BackupProvider`, including the
Proton CLI adapter. `build/omacustos-incremental` is a real, explicitly invoked
entry point, not a cloud-test stub. The ordinary worker still makes v3 full copies;
incremental default rollout waits for restore/discovery (#59), GC/full integrity
maintenance (#60), and release validation. The new executable is built but is not
added to the packaged default launchers or schedules.

## Approved ownership change

The user explicitly approved the manual offline handoff model on 2026-10-20 after
the installed `cli-drive@0.8.0` help and official source showed no remote per-set
fencing API. The installed CLI also reports 0.9.0 available; no claim is made that
a CLI upgrade supplies a fence. Current upload commands expose conflict choices,
not an expected-owner token. The SDK's revision precondition is narrower than
repository-wide fencing across upload/trash/delete.

The [amended contract](incremental-repository.md#ownership-and-operational-coordination)
requires one owning computer, disabled old writers before handoff, and one shared
local coordination directory. Every execution/provisioning operation holds a
`QLockFile` keyed by **set UUID**, even if paths/configurations differ. Its default
location is `~/.config/omacustos/incremental`. Supplying a different state directory
is for isolated tests/controlled deployments, not a way to run parallel writers.
The anchor is downloaded and exactly checked before every remote write. Wrong
computer/epoch/origin, stale continuation identity or malformed history stops work.
An unfinished remote operation prevents starting a different operation in that set.

These checks **cannot remotely fence a stale process** between check and mutation.
Do not clone an active identity, run on two computers, use two coordination roots,
or hand off while the old machine could restart. No expiring remote lock is used.
The manual handoff procedure is specified by #56; user-facing takeover tooling is
part of #59. Do not edit the anchor as a substitute for disabling the old writer.

## Advanced CLI use

This is a non-default development/advanced-user seam. Provision a **new empty**
set namespace; do not point it at legacy copy folders. Generate and preserve a
computer UUID once and a set UUID once. Generate fresh operation/snapshot UUIDs for
each new logical run; retries/resume use the **same** pair. Use canonical UUIDv4
strings. Keep the request file private and outside the selected sources.

Example request (replace paths, IDs and sources deliberately):

```json
{
  "remote_set_root": "/my-files/backups/sets/10000000-0000-4000-8000-000000000001",
  "set_id": "10000000-0000-4000-8000-000000000001",
  "computer_id": "20000000-0000-4000-8000-000000000001",
  "origin_computer_id": "20000000-0000-4000-8000-000000000001",
  "ownership_epoch": 1,
  "operation_id": "50000000-0000-4000-8000-000000000001",
  "snapshot_id": "30000000-0000-4000-8000-000000000001",
  "staging_directory": "/home/USER/.local/share/omacustos/staging",
  "staging_budget_bytes": 1000000000,
  "archive_target_bytes": 1000000000,
  "batch_file_limit": 1000,
  "sources": [{"source": "/home/USER/project/main.cpp", "restore": "project/main.cpp"}]
}
```

```bash
build/omacustos-incremental --request request.json --initialize --acknowledge-single-writer
build/omacustos-incremental --request request.json --acknowledge-single-writer
```

`sources` is an explicit completed selection with stable restore mappings, not
directories to recursively expand. Missing files, symlinks, unsafe/colliding mappings
or unreadable sources fail. The library also accepts scanner issues, which fail the
whole plan. Use the existing scanner's inclusion/exclusion/mapping rules when
integrating into the eventual default worker. No local previous manifest is needed:
the executor validates all published snapshot manifests and selects the latest by
creation time/UUID. A supplied previous snapshot must agree with that discovery.

SIGINT/SIGTERM cooperatively pause and stop owned CLI children; rerun the same
request to resume. `--cancel` durably abandons that operation without deleting its
objects. A prepared publication can still be cancelled; an already committed
manifest wins a late cancel. An immutable terminal operation is idempotent: rerunning
it reports its original completion/check timestamp, not a new check. Generate fresh
operation/snapshot IDs for the next check. JSON output exposes saved/unchanged/
paused/cancelled/waiting/retrying/failed, committed manifest path, separate check
and saved-change timestamps, issues, damaged object IDs and affected snapshots.

## Durable continuation protocol

This is a separate payload-format-4 continuation namespace; the copy-scoped
`BackupContinuation` v2/v3 journals are neither migrated nor reinterpreted.
Local journals live below `<shared-state>/<sha256(exact-set-root)>/<operation-id>`;
remote recovery uses the following immutable records:

```text
operations/<operation-id>/
  identity.json                 # version 1, payload_format 4, all IDs/root/epoch/time
  reuse-<uuid>.json              # version 1, protected complete archive descriptors
  prepared-<object-id>.json      # version 1, archive descriptor without receipt
  verified-<object-id>.json      # version 1, same descriptor plus original receipt
  publication.json              # exact complete v4 manifest, immutable final intent
  complete.json                 # version 1, saved/unchanged, timestamps and snapshot
  cancelled.json                # version 1, explicit terminal abandonment
```

Local prepared checkpoints use `QSaveFile` without direct-write fallback, syncing
directory links/renames. Remote preparation is uploaded and read back before its
object. Source capture uses the planner's no-follow descriptor walk, bounded reads,
stat consistency and exact hash/mode/nanosecond-mtime comparison with the plan.
Captured files are private/read-only; gzip level 3 packing rehashes them. Archives
are read-only before upload, with new random IDs and complete physical inventories.

The normal working-space bound includes raw captures, conservative tar/gzip output,
**and a second compressed download for full verification**, plus the existing
filesystem metadata reserve. Batches obey both byte and file-count limits. One
whole oversized file may exceed the soft budget after free-space preflight and an
explicit oversized-file checkpoint; no file is split. Multi-member recovery
downloads exceeding a lowered budget enter Waiting. Actual quota/write/flush/
checkpoint failures stop without releasing any retention authority.

Objects and control records never use upload replace/rename/new-revision. Proton
JSON-mode unresolved conflicts fail; its automatic identical-content skip is
accepted only after mandatory verification. Every newly uploaded or merely
prepared-but-recovered object is **downloaded**, checked for compressed size/SHA-256,
complete gzip/tar structure and exact full physical membership/size/SHA-256. This
uses a validation-only archive reader; v3 extraction's existing strict behavior
is unchanged. Current Proton metadata is conservatively not declared a trustworthy
SHA-256 authority. A provider can explicitly certify its content-checksum semantics;
the local test provider computes current file bytes.

After full verification: remote original-evidence record → readback → local synced
verified checkpoint → release staging. Normal source snapshots and compressed output
remain through that checkpoint. Pause/failure unwinds private staging as existing
backup recovery does; **remote protection remains**. A prepared descriptor alone
never authorizes reuse. Resume inspects exact object IDs and re-verifies prepared
uploads, rather than overwriting or trusting their size. Remote verified evidence
and complete inventories reconstruct continuation after local journals are lost.
Re-scan current selection until the immutable publication intent exists. After that
point, recovery finishes that captured state, not newly changed local source bytes.

Daily reuse requires the durable original receipt, fresh existence/content size,
and a matching trustworthy checksum where supported. It does not download all
reused archives. **Size-only checks can miss same-sized corruption until full
integrity maintenance or restore.** An unchanged successful check creates only a
terminal operation/check record, not another snapshot or retention slot.

For confirmed missing/corrupt reused data, current local files are repacked into
fresh objects. Report all saved snapshot IDs referencing each damaged object and
preserve their manifests; no historical repair is claimed. Network/authentication,
malformed or ambiguous listing/metadata and unreadable history are unknown health,
not absence. They stop execution without guessed replacements or cleanup. Conflicting
immutable descriptors are corruption and stop work.

## Publication and cleanup protection

All new verified and reused healthy references must pass before publication intent
is prepared, synced locally, uploaded and read back remotely. The complete v4
`snapshots/<snapshot-id>/manifest.json` is uploaded **last**. Readback must match the
prepared document and schema before durable local success, then remote completion.
A lost upload response is reconciled by exact identity/content on resume. Never
rewrite a published snapshot or broaden an immutable publication intent. If intent
references subsequently damaged data, cancel/start a new operation rather than
silently changing what was committed.

All operation/reuse/prepared/verified/publication records protect their object
references while unfinished. Pause has no age expiry. Cancel needs a durable marker;
a lost cancellation response is retried, not assumed successful. Completed snapshots
take over their own live references; terminal leftover objects become candidates
only for future reference-aware GC, never immediate deletion by this executor.

This module **never calls retention, trash or permanent deletion**, on success or
failure. It cannot make room by deleting history. Local/remote quota exhaustion
preserves all old states. Do not point legacy copy cleanup at `sets/<UUID>`;
future #60 cleanup must understand every protected record and share the set UUID
lock. Until #59/#60/release gates pass, the ordinary application does not discover,
restore or retain v4 copies and incremental uploads are not the default.

## Automated coverage

`incrementalrepository-test` injects interruption at preparation, protection,
upload, verification, remote/local checkpoint and publication boundaries. It tests
lost responses, remote evidence recovery after local-state loss, current-source
changes versus frozen publication, exact subset inventory retention, changed-only
uploads, no-download daily checks, the deliberate same-size-corruption limitation,
fresh-ID damage replacement and affected history, quota failures, cancel, owner/
epoch changes, local exclusion, protected work and empty states. A complete offline
Proton-adapter run uses the CLI filesystem fixture, including noninteractive
conflict-failing uploads. No cloud mutation is needed for these tests.

Review-driven regressions also cover an identity upload failing **before** remote
creation with a changed retry/check time (resume and cancel), strict terminal
record fields/timestamps/snapshot consistency, and lowered-budget recovery. Reuse
matching builds previous logical-reference and recovered-object hash sets once;
it does not scan all logical entries for each physical archive member.

Future GC must reconcile or retire terminal operation/check metadata together with
removed snapshots; stale completion records pointing at missing or mismatched
history currently fail closed rather than authorizing success or new writes.

### Validation record (2026-10-20)

- `cmake -S native -B build` and `cmake --build build -j 4`: passed; existing
  optional Vulkan/QML-policy and legacy Qt-deprecation warnings only.
- `build/incrementalrepository-test`: **41 passed, 0 failed**, including the
  offline Proton adapter and all review-driven recovery regressions.
- After resolving review findings, `ctest --test-dir build --output-on-failure
  -j 4 -E '^workerrecovery-test$'`: **26/26 targets passed**, 85.16 seconds.
- `build/workerrecovery-test` with a 360-second external allowance: **139 passed,
  0 failed**, 182.567 seconds. The previously documented 180-second CTest limit
  was not changed; this suite was run separately to allow it to complete.
- Independent pre-commit review of the staged scope against `2fc1ec6`:
  **Standards: no violations; Spec/safety: all three findings resolved and
  re-reviewed, no remaining blockers**. Fixes cover pre-upload identity recovery,
  strict terminal record validation, and quadratic reuse matching. Both reviewers
  reran the four relevant provider/schema/planner/repository CTest targets: **4/4
  passed**. `git diff --cached --check` passed.
- `build/omacustos-incremental --help` passed. No live cloud mutation was performed;
  the offline provider/CLI fixtures are not a claim of cloud certification.
