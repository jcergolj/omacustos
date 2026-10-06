# Archive backup acceptance (#44)

The archive rollout is delivered by #45–#52. Fresh manual and scheduled copies
use version-3 bounded archives; historical version-1/version-2 copies and
unfinished individual-file attempts retain their original format.

## Acceptance evidence

- #45/#46: operation-owned selective restore, versioned manifest validation,
  immutable archive contents, and safe extraction. See `singlearchive.md`.
- #47/#49: bounded preparation across selected roots, whole oversized files,
  storage Waiting, verified Incomplete copies, and success-only mixed-format
  retention. See `boundedarchives.md`.
- #48: unique-archive download cost and selective downloads through the public
  restore controller, with QML selection/cost coverage.
- #50: durable archive continuation through pause, cancellation, process death,
  transfer failures, and checkpoint interruptions. See `archiverecovery.md`.
- #51: source edits/deletions/exclusions, stable root mappings, and missing or
  damaged remote archive reconciliation. See `archivereconciliation.md`.
- #52: archive-only fresh defaults, historical compatibility, remote-operation
  scaling, and byte-validated resource/compression comparisons. See
  `archiverollout.md`.

`MANUAL.md` documents gzip level 3, selective-download amplification, staging
and the oversized-file exception, legacy compatibility, and full-copy daily
behavior. The resource measurements in `archiverollout.md` are local fixture
observations, not Proton network throughput guarantees.

## Completion verification

On 2026-10-06, `cmake --build build -j 4` succeeded. All 24 registered native/QML
test executables passed with `QT_QPA_PLATFORM=offscreen`:

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure -j 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure -R '^workerrecovery-test$'
```

The first invocation passed 23 executables before the command harness's
120-second timeout interrupted the long worker test. The second invocation
ran that executable to completion with a longer timeout and passed in 161.65
seconds. This records a split verification run rather than claiming the
interrupted invocation itself completed successfully.
