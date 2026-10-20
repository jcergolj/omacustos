# Shared-archive recovery (#59)

The advanced `build/omacustos-incremental` entry point discovers/restores v4
snapshots with only an authenticated Proton CLI and a selected remote root. No
local settings, sources, schedules or journals are required. Default worker
rollout and shared-object cleanup remain gated on #60 and release validation.
`BackupManifest` still reads only v1–v3; `IncrementalRestore` is the separate v4
schema/location/anchor adapter used by `BackupCatalog::verifyCopy`.

```sh
build/omacustos-incremental --discover /my-files/Backups
build/omacustos-incremental \
  --snapshot /my-files/Backups/sets/SET_UUID/snapshots/SNAPSHOT_UUID \
  --destination "$HOME/Recovered"
# Repeat --select for multiple exact logical paths; omit it for all files.
build/omacustos-incremental \
  --snapshot /my-files/Backups/sets/SET_UUID/snapshots/SNAPSHOT_UUID \
  --destination "$HOME/Recovered" --select documents/report.txt
```

Discovery reads only direct UUID repositories below `sets` and direct UUID
snapshots. Missing manifests are unpublished; malformed/foreign manifests appear
damaged with no restorable files. Missing or metadata-mismatched archives make
their logical files unavailable. Discovery metadata is not full byte verification.
Restore downloads each needed archive once and checks compressed size/SHA-256,
complete gzip/tar structure and every physical member's name, regular-file type,
size and SHA-256, including obsolete/unselected members. Extra, duplicate, linked
and unsafe members are rejected. Only selected files are materialized. No ancestor
manifests are read.

Restore preserves bytes, relative destination, POSIX mode bits 0777 and nanosecond
mtime. It does not preserve owners/groups, ACLs, xattrs, links, sparse allocation,
special files, directory metadata or empty directories. Use a fresh destination
to reproduce a saved file list: unrelated existing files are not deleted.
Mode/time are applied and verified on the atomic temporary file before placement;
unsupported precision fails explicitly. Symlink containment checks and disabled
direct-write fallback remain in force. Already committed files remain placed if a
later file fails (existing partial-restore behavior).

## Replacement computer / offline ownership handoff

Discovery/restore does not grant writes. There is **no server-enforced fencing or
safe online takeover** in the installed CLI. The approved #58 manual model applies:

1. Inspect the exact `repository.json` root, owner and epoch.
2. Stop the old writer, schedules/services, transfers and retries.
3. Prevent restart by disabling/removing old writer configuration or revoking its
   access. If this cannot be secured, remain read-only.
4. Reconcile/cancel unfinished operations under the old owner first. Handoff
   refuses unfinished/conflicting operations and maintenance records. Neither age
   nor lost local journals authorizes abandonment or deletion.
5. Prepare a request identifying the **current** set, origin, owner and epoch,
   fresh operation/snapshot UUIDs and shared disk-backed state/staging directories
   per [execution](incremental-execution.md). `sources` may be omitted for takeover.
   Generate a fresh replacement computer UUID; do not clone the old identity.

```sh
build/omacustos-incremental --request current-owner.json \
  --takeover NEW_COMPUTER_UUID --acknowledge-single-writer \
  --confirm-old-writer-disabled
```

Handoff holds the shared local mutation lock, rereads the exact old anchor,
checks protected operations, durably checkpoints before/after intent, updates only
the mutable anchor and verifies fresh readback. Origin stays unchanged; epoch
increments. Unknown/conflicting updates require read-only inspection, not blind
repetition. No schedules are recreated/enabled, no journals migrate epochs and
no archives, snapshots or protection records are deleted. Explicitly update the
new writer request before enabling it. Old identity requests fail owner checks;
these checks are **not an atomic server fence** and do not replace operator
isolation of the old installation.

## Validation

`incrementalrestore-test` covers multiple archive generations, old/new states,
edits/deletions/moves/renames, lost sources/journals, selective restore, multiple
destinations per member, exact metadata, corrupt/missing archives, malformed
manifests/provenance/locations, unselected-byte corruption, extra/duplicate/unsafe/
linked members, gzip truncation/trailer damage, destination symlinks, unauthorized
or unfinished takeover and read-only Proton CLI fixture integration. No live
cloud mutations are performed by the suite.

### Delivery checks (2026-10-20)

- Full CMake build passed (existing optional Vulkan/QML-policy warnings only).
- CTest excluding the separately run recovery suite: **27/27 targets passed**.
- Direct `build/workerrecovery-test` with a 360-second external allowance:
  **139 passed, 0 failed**, 178.288 seconds. An initial run overlapped rebuilding
  and hit worker-launch failures; this rerun ran without relinking its executable.
  The existing CTest timeout was not changed.
- After fixing the Standards review's legacy `sets` computer-name regression,
  all **8 affected suites passed**; `incrementalrestore-test`: **31 passed**.
- Subprocess CLI smoke checks using the offline Proton fixture passed: remote-only
  discovery, selected restore after source deletion with no mutation commands,
  explicit offline handoff with no source mappings, and historical restore after
  handoff. Conflicting operation options and unauthorized takeover were rejected.
- Independent pre-commit Standards and Spec/safety reviews against
  `0fb6e4bcc2286c07137c63b02621a86c1e7a41b9`: no remaining blockers. The legacy
  discovery finding was fixed, regression-tested and independently re-reviewed.
- `git diff --cached --check` passed. No live cloud mutation was performed.
