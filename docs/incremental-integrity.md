# Scheduled integrity and backup health (#61)

> Historical component delivery record. Normal workers now advance this protocol
> too; the UI also offers a full check. See [current integration](incremental-default-rollout.md).

The advanced `omacustos-incremental` executor advances full integrity maintenance
**before** each backup and before starting cleanup. This does not enable incremental
backups in the ordinary v3 worker or install a new system schedule. Invoke the
advanced executor daily using the same owning installation and shared state path.

## Schedule, budget and coverage

A missing remote ledger is due immediately; otherwise a cycle starts 30 days after
the last successful completion. `--check-integrity --request request.json
--acknowledge-single-writer` starts a cycle immediately or resumes the unfinished
one. Each invocation processes one archive at a time, stopping between objects
after one hour or 10 GiB of archive downloads. Request settings
`integrity_time_budget_ms` and `integrity_byte_budget` override those positive soft
limits. One archive may exceed the remaining slice. The existing staging budget,
free-space checks and single-oversized-file exception remain enforced; the report
names oversized single-file objects. Oversized multi-member archives enter Waiting.

Every retained manifest is schema-validated before work. The sorted worklist
includes every distinct physical archive referenced by retained snapshots and
unfinished operation preparations, verifications, reuse and publication intents.
Prepared uploads are included even without an original receipt; missing prepared
bytes cannot be certified. Shared objects count once, with their entire physical
size/inventory, not just the subset a snapshot uses. Conflicting descriptors stop
the check. Completion re-enumerates reachability and verifies known revisions again;
new references are included on resume and only proven unreachable objects disappear.

Full verification downloads and checks SHA-256, gzip/tar structure and every
physical member. This deliberately also downloads checksum-capable objects on their
first check in each cycle, rather than assuming their physical structure. Fresh
trustworthy checksums can detect damage before a download. Daily reuse remains
original-verification evidence plus existence/size and trustworthy checksums when
available. **Without a trustworthy checksum, daily checks cannot detect same-sized
corruption.** Daily checks never refresh original full-verification timestamps.

## Durable evidence and recovery

Append-only records, separate from deletion proposals:

```text
integrity/<cycle UUID>/plan.json
integrity/<cycle UUID>/checks/<object UUID>/<binding SHA-256>.json
integrity/<cycle UUID>/complete.json
integrity/<cycle UUID>/cancelled.json
integrity-damage/<object UUID>.json
```

The version-1 plan records owner/epoch, cycle ID, start time and sorted complete
physical descriptors. A coverage receipt binds that cycle, exact descriptor,
observed content revision/size, method, timestamp and good/damaged result. Each
receipt is uploaded immutably and read back before syncing the local cursor.
Loss of a local cursor uses remote evidence. Failed receipt uploads are rechecked;
a lost upload response is reconciled against the exact persisted receipt. Completed
records embed exact final coverage, count, bytes, plan hash, completion time and
explicitly reconciled unreachable plan descriptors. Validation requires every
planned object to be covered or proven currently unreachable; receipt bindings
are recomputed, not just checked for plausible formatting.
Malformed or ambiguous evidence fails closed, never resetting the schedule to a
false success. Missing ledger restarts full coverage.

Proton's `activeRevision.uid` supplies content revision identity, **not** a trustworthy
SHA-256. The SDK's [NodeEntity/Revision interface](https://github.com/ProtonDriveApps/sdk/blob/main/client/js/src/interface/nodes.ts)
defines this field; no node UID or modification timestamp is substituted. The
local provider uses its freshly computed content digest as revision identity.
Resume freshly inspects revisions before reusing receipts. A changed revision gets
a fresh download. Providers supplying neither revision identity nor trustworthy
checksum must re-download previous coverage after interruption; the report warns
that a larger budget may be required to finish in one invocation. This conservative
fallback never treats unknown revisions as unchanged.

SIGINT/SIGTERM pause cooperatively. Resume uses the same unfinished cycle, with
current reachability reconciliation. `--cancel-integrity` cancels coverage without
deleting objects; the next invocation starts a fresh cycle. A cancelled cycle is
not successful coverage. Backup, maintenance, takeover and cleanup share the set
mutation lock and exact owner checks. Offline takeover requires finishing or
cancelling an active cycle; stale-epoch continuations cannot resume. Existing
authorized deletion transactions must be reconciled before integrity maintenance;
new cleanup is blocked by unfinished, damaged or unknown integrity health.

## Health and repair

Backup JSON reports `last_successful_check` and `last_saved_change` separately.
`integrity` contains status, cycle/start, last completion/next due, overdue, current
coverage count/bytes, remaining objects, downloaded bytes, damage, affected historical
snapshot IDs, errors and `cleanup_blocked`. `last_coverage` on a not-due report
describes the last completed cycle, not a claim that newly saved objects were in it.
`cleanup_needs_attention` and cleanup issues are separate from backup success.
Maintenance failure does not erase backup timestamps or prevent a current-source
backup; known damaged objects cannot be reused even by a size-only provider.

Retry transient authentication/network/staging failures with the same request.
For corruption, inspect affected snapshots and restore any recoverable files
independently. Run a fresh backup operation with current local sources to create
replacement **new IDs**. Old snapshots remain damaged and cleanup remains blocked;
replacement uploads do not repair history. There is no automatic compaction,
historical-manifest rewrite or archive overwrite. Cancelling/restarting maintenance
does not bypass checking those still-retained damaged references.

Confirmed damage is also saved independently of cycles, in the append-only remote
`integrity-damage` ledger and a synced shared local checkpoint. Cancel, budget
expiry and failures cannot make a known-corrupt ID reusable. Local evidence retries
a failed remote damage publication; unreadable damage knowledge forbids backup
reuse. The blacklist is not cleared by writing a new revision under an old ID.

## Validation

Automated coverage lives in `native/tests/incrementalrepository_test.cpp`: checksum
availability and same-size damage, all affected historical snapshots, byte/time
slices, pause/cancel, missing ledger/local cursor, lost responses, auth/download
failure, changing revisions, protected uploads, newly reachable objects, coverage
accounting, staging wait/recovery, separate backup health and cleanup blocking.
Review-driven regressions also cover truncated completion coverage, invalid receipt
bindings, conflicting original receipts, cancelled coverage losing corruption
knowledge, and daily corruption followed by replacement-upload or owner-read failure.

### Validation record (2026-10-20)

- `cmake --build build -j 4`: passed.
- `ctest --test-dir build --output-on-failure -j 4 -E '^workerrecovery-test$'`:
  **27/27 targets passed**, 83.77 seconds.
- After the final local-before-owner-check fix, rebuilt and reran the provider,
  incremental manifest, planner, restore and repository CTest targets:
  **5/5 passed**, 34.17 seconds. Repository suite: **92 passed, 0 failed**.
- `build/workerrecovery-test`, run separately with a 360-second allowance:
  **139 passed, 0 failed**, 167.934 seconds. Its existing CTest timeout was unchanged.
- CLI help and mutually exclusive integrity/cleanup option validation passed.
- Independent pre-commit Standards and Spec/safety reviews against `74ff38e`:
  all blockers resolved and re-reviewed; **no remaining blockers**. Regression
  coverage includes every review finding. `git diff --check` passed.
- No live cloud mutation was performed; provider/CLI fixtures are offline evidence,
  not cloud certification. The ordinary v3 worker/default rollout remains unchanged.
