# Default incremental rollout decisions (#62)

Confirmed with the user on 2026-10-20. These decisions do not certify release
readiness or authorize cloud access or deployment.

## Cleanup authorization

Scheduled backups must not automatically confirm their own cleanup proposals.
The worker saves the backup and reports cleanup needs attention. A user-facing
preview must present the exact deletion proposal for explicit confirmation before
the reference-aware executor deletes anything. If the proposal changes, obtain
fresh confirmation; do not substitute a general newest-N policy authorization.

## Writer provisioning

Only a verified-empty remote set may be provisioned automatically. Bind its stable
writer UUID to the local installation, not an editable hostname or display name.
Imported or copied configurations remain read-only for discovery and restore
until explicit offline takeover confirms the previous writer is permanently
disabled. Missing local state must never silently authorize adopting existing
remote history or creating a replacement writer over it.

These policies are now implemented and covered by worker/controller failure
tests. See [current integration and delivery evidence](incremental-default-rollout.md).

## Source enumeration prerequisite

Directory open/read errors must be distinguishable from successful enumeration
of an empty directory. The scanner now exposes `BackupPreview::scanErrors`;
both existing backup execution paths refuse publication when this list is
nonempty. The preview UI includes these paths among skipped paths. Errors while
browsing explicitly excluded directories for preview do not invalidate the
selected files.

`scanerrors-test` injects both directory-open failure and a directory-read error
after a real entry has been delivered. It checks that neither legacy nor bounded
archive execution publishes a manifest, even if other selected files are readable.
It also checks normal hidden-file enumeration and excluded-directory behavior.
Real permission-denial cases additionally cover selected roots and descendants
without read or directory-search permission, across all three execution modes.
Those cases require an unprivileged test process.

The incremental selection adapter now carries enumeration errors, missing roots,
and unreadable selected paths into `IncrementalPlanRequest::scanIssues`.
Intentionally skipped links/special nodes remain outside regular-file selection.
An incomplete scan cannot authorize removals. The scanner-only results below
remain historical preparation, not the final rollout evidence.

## Validation of this preparatory change

- `cmake --build build -j 4` succeeded before the review follow-up; the affected
  scanner, engine, controller, and worker targets were rebuilt after the follow-up.
- `ctest --test-dir build -R '^(scanerrors-test|backupengine-test|backupsetcontroller-test)$' --output-on-failure`:
  **3/3 suites passed**, 5.28 seconds, after the review follow-up.
- Separate standards/spec reviews found two gaps: the original test rows did not
  actually exercise the legacy non-fresh execution path, and unreadable selected
  directories bypassed the new enumeration-error guard. Both were fixed and
  re-reviewed; neither reviewer found remaining blockers in the scanner-only
  scope. Both explicitly retained the full #62 release blockers.

Those preparatory results alone were not a successful default-rollout review.
The full 41-suite run, controlled benchmarks, and final reviews are recorded in
the [delivery report](incremental-default-rollout.md).
