# Incremental backup delivery (#63)

This report consolidates delivery of the incremental-backup tracking issue.
It does not authorize deployment, installed-service changes, or cloud access.

## Implementation on master

All seven child issues are closed and their implementation commits are ancestors
of `master`:

| Issue | Delivered boundary | Commit |
| --- | --- | --- |
| #56 | Repository format and safety protocols | `e20ef90` |
| #57 | Deterministic whole-file incremental planner | `2fc1ec6` |
| #58 | Shared uploads and manifest-last publication | `0fb6e4b` |
| #59 | Shared-archive restore and remote-only recovery | `036c03a` |
| #60 | Reference-aware retention and recoverable cleanup | `74ff38e` |
| #61 | Scheduled full integrity and backup health | `ae7a920` |
| #62 | Default worker integration and measured archive sizing | `f4ccf95` |

The current implementation and correctness coverage are described in
[Default incremental backups](incremental-default-rollout.md). The earlier
[validation report](incremental-validation.md) is explicitly historical; its
preparatory blockers are not the final delivery status.

## Correctness acceptance

Final verification is against `f4ccf95`, covering the combined implementation
with `git diff 7485d32...HEAD`. This closure adds delivery evidence only; the
automated regressions were delivered in the child commits above.

Commands used for the final Release rebuild and complete test run:

```sh
cmake -S native -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
ctest --test-dir build --output-on-failure -j 2
```

On 2026-10-21, the Release rebuild succeeded and the complete test invocation
passed **41/41 suites in 455.03 seconds**, including all three benchmark smoke
cases. No test cases were dropped, timeouts raised, or integrity/deletion
safeguards weakened. Existing nonfatal Qt configuration/deprecation warnings
remain unrelated to this documentation-only closure.

### Independent pre-commit review

- **Standards:** no confirmed documented-standard violations or delivery
  blockers. A nonblocking consolidation suggestion concerned duplicated anchor
  validation in takeover and restore. Follow-up tracing confirmed that the
  repository layer rejects unsafe epochs and requires exact owner identity
  before remote mutation; no correctness defect was established.
- **Spec:** no verified missing, partial, incorrectly implemented, or out-of-scope
  requirements. The reviewer independently reran the manifest, planner,
  repository, restore, and worker suites: **5/5 passed in 44.53 seconds**.

Both reviews covered the combined seven-child implementation before the closure
commit. Final correctness/performance evidence is also recorded on #63; this
report is delivered by its issue-closing commit on `master`.

## Performance evidence

The [final local measurements](incremental-default-rollout.md#final-local-execution-evidence--2026-10-21)
contain 18 successful invocations: two repeats for three profiles at three archive
targets, including v3/v4 baseline, unchanged, edit, full/selective restore, cleanup,
and retained-state restore. On 2026-10-21, the closure check independently parsed
all 252 JSON records in the 18 local raw evidence files and reproduced their
documented receipt SHA-256:

```text
8e14a0fe161d0919f99280aee3f7fb93839b03c3fd0117d59426e7040e84ea7e
```

Raw files remain local at `/tmp/opencode/issue62-final-20261021`; the linked
report preserves the results and measurement boundaries in the repository.
At the retained 1,000 MB requested archive target, the 512 MiB amplified-code
fixture took 1.856–1.936 s for unchanged v4 checks versus 5.066–5.394 s for v3;
the mixed fixture took 2.296–2.465 s versus 10.687–10.757 s. Unchanged runs
uploaded no payload objects and created no snapshot. Small edits uploaded one
whole changed file. New baselines were slower, and fixed overhead made the small
unamplified repository fixture slower with v4. These are local-provider results,
not measured cloud speedups.

The approximately 1 GB requested target, existing 1 GB staging budget, and
1,000-file cap remain unchanged. Smaller targets reduced selective transfer but
increased remote operations; the linked report records the measured tradeoff.
20/50/100/500 GB fixtures were impractical under the available disk capacity and
the conservative fixture guard. No cloud measurements were performed.

## Preserved limits and safeguards

- All selected regular files are hashed; capture is file-by-file, not an atomic
  application/filesystem snapshot. Changed files are transferred whole.
- New uploads are fully verified. Daily reuse checks backed by original
  verification evidence can miss same-sized remote corruption; scheduled full
  integrity checks and verified restores remain necessary.
- Each saved state is independently restorable, including legacy history.
  Shared physical membership remains distinct from logical snapshot membership.
- Ownership takeover is explicit and manual-offline: the previous writer must
  be permanently stopped. This is not server-enforced fencing.
- Safe cleanup requires exact-scope confirmation, retains newest-N saved states
  across formats, and protects retained/resumable references. Unchanged checks
  consume no slot. Retained history is never removed to make upload space.
- The first new-format run uploads a fresh baseline with capacity estimates.
  Replacing damaged objects for a new state does not repair older snapshots.
- Partially live archives keep obsolete members; compaction and block-level
  deduplication remain deferred.
