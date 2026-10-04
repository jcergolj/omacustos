# Responsiveness measurements — issue #22

## Run the opt-in benchmark

```sh
cmake -S native -B build
cmake --build build --target responsiveness-benchmark
TMPDIR="$PWD/build" QT_QPA_PLATFORM=wayland \
  QT_QUICK_CONTROLS_STYLE=Fusion QT_QPA_PLATFORMTHEME= \
  OMACUSTOS_BENCHMARK_SAMPLES=50 build/responsiveness-benchmark -nocrashhandler
```

Run on the real desktop backend, with the window visible. `TMPDIR` must be an
existing writable directory on the intended test filesystem. The default `/tmp`
on this machine is **tmpfs**, so omitting it measures memory-backed fixtures,
not NVMe/SATA I/O. The JSON output reports the fixture filesystem, temporary
parent, graphics API, memory-pressure allocation, and cgroup memory events.

For a separate constrained/pressure scenario on this development machine:

```sh
systemd-run --user --scope -p MemoryHigh=384M -p MemoryMax=1G \
  taskset -c 0,1 env TMPDIR="$PWD/build" \
  QT_QPA_PLATFORM=wayland QT_QUICK_CONTROLS_STYLE=Fusion QT_QPA_PLATFORMTHEME= \
  OMACUSTOS_BENCHMARK_SAMPLES=50 OMACUSTOS_BENCHMARK_MEMORY_MB=512 \
  "$PWD/build/responsiveness-benchmark" -nocrashhandler
```

Choose CPU IDs available on the test machine. These are temporary restrictions
on the benchmark process; they do not modify application or desktop settings.
The allocated memory is touched, then retained for the scenario. Check the
reported `memory.events` high count to distinguish actual cgroup pressure from
an allocation that never reaches its limit.

The benchmark is intentionally not registered with CTest. Deterministic
correctness/nonblocking regressions remain in `backupsetcontroller-test`,
`backuprestorecontroller-test`, and `dashboard-test`.

## Method

- Dataset: 20 saved backups and 100 direct copy directories per backup. One
  selected copy contains 10,000 real files, each with a 256-byte identical
  payload, and a production-engine-generated manifest. Other copy directories
  exercise listing size; they are not 2,000 full payload replicas.
- 50 samples per interaction and workload. Report each workload separately;
  p95 is the nearest-rank 48th value of 50 sorted observations. No warm-up
  samples are discarded after interaction measurement starts.
- Real `Main.qml`, `BackupSetController`, `BackupRestoreController`,
  `BackupEngine`, and `LocalProvider` operate on isolated temporary state.
  Auth, systemd/scheduling, browser integration, and resource-setting services
  are doubles. No user configuration or remote account is accessed.
- **Idle:** no active transfer or scan.
- **Backup:** the production backup engine repeatedly scans, stages/hashes,
  uploads, and verifies 10,000 files in a background task. A new pass starts
  after the preceding pass finishes. The reported pass count counts completed
  passes, not whether work is active. A count of zero can mean the first pass
  was still running during all interactions. The GUI run-history fixture remains
  the last successful snapshot; this scenario does not measure live systemd
  worker progress writes/polling or Proton CLI subprocess contention.
- **Restore:** the real controller restores all 10,000 verified files. If a
  transfer finishes between samples it is reverified and restarted before the
  next interaction. Navigation can enqueue copy listing behind the transfer.
- **Scan:** a real `preview()` request is dispatched inside the measured
  interval, immediately before the click/key interaction. This includes scan
  dispatch plus input feedback, so synchronous scanning cannot be hidden by
  starting the timer after it blocks.
- Selection uses an actual checkbox click with 100 existing ticks in a
  10,000-file model. The separate regression test exercises 10,000 ticks,
  offscreen delegates, return scroll, reordered entries, and removed entries.
- Editor navigation starts at clicking **Edit**, with the overflow menu already
  open. Restore navigation starts at clicking the dashboard **Restore** button.
  Typing starts at key dispatch to the editor name field. Navigation checks
  local visibility/focus; copy discovery and verification completion are outside
  the navigation interval.
- Timings end at the next Qt `frameSwapped` after dispatch/update. QtTest input
  is injected directly, bypassing physical input/compositor delivery. This
  measures Qt frame submission/event processing, **not compositor presentation
  or physical visible-feedback latency**. Event-loop/harness scheduling and
  frame pacing contribute to the approximately 50 ms cluster. These timings
  are conservative development proxies, not proof of the visible budgets.
- Successful restore completion can close the panel before a navigation frame.
  The harness reports those samples separately as
  `restore_navigation_completed_at_boundary`, and reports disabled navigation
  attempts rather than pretending they passed a timing budget. It also reports
  completion during selection; no such samples occurred in the disk-backed runs
  below. Initial tmpfs probes did encounter this boundary, which is why it is
  handled explicitly.

## Recorded runs, 2026-10-03

**Hardware/environment:** Intel Core i5-9600K, six physical cores/six logical
CPUs, approximately 47 GiB usable RAM, Intel UHD Graphics 630, WDS500G2X0C NVMe
SSD, Btrfs with `compress=zstd:3`; Omarchy 4.0.4 / Hyprland / Wayland; Qt 6.11.2,
GCC 16.2.1; Fusion controls; Qt Quick OpenGL (graphics API 3), normal graphical
rendering, not offscreen/software. Fixtures and staging used the workspace
`build` directory on Btrfs/NVMe. Files are small/compressible and the selected
copy is verified before interaction sampling, so filesystem caches are warm.

**Baseline:** detached worktree at `daf93ff`, before issue #22 implementation
changes. The same benchmark source/target was added to that worktree; its
production controllers and QML were unchanged. To reproduce the comparison,
copy the benchmark source and its CMake target stanza into a worktree at that
revision and build/run it with the same environment and filesystem.

**After:** final issue #22 working-tree implementation based on `daf93ff`,
including the dropdown binding, in-view return-focus, and time-throttled restore
progress fixes. The final on-screen runs also asserted editor visibility and focus.
These recorded timings predate the follow-up worker/dashboard optimizations below.

All values are milliseconds, rounded to one decimal. Each row has 50 samples.

| Workload | Interaction | Before p95 | Before worst | After p95 | After worst |
| --- | --- | ---: | ---: | ---: | ---: |
| Idle | Checkbox selection | 248.4 | 253.1 | 50.9 | 51.3 |
| Idle | Editor navigation | 9.5 | 37.6 | 8.0 | 12.6 |
| Idle | Typing | 15.8 | 16.7 | 14.8 | 18.0 |
| Idle | Restore navigation | 50.9 | 65.4 | 49.3 | 53.8 |
| Backup | Checkbox selection | 280.8 | 284.3 | 50.8 | 52.2 |
| Backup | Editor navigation | 12.2 | 50.2 | 8.8 | 12.4 |
| Backup | Typing | 16.6 | 16.9 | 14.6 | 17.1 |
| Backup | Restore navigation | 51.2 | 65.4 | 49.6 | 65.5 |
| Restore | Checkbox selection | 332.8 | 333.8 | 50.6 | 60.9 |
| Restore | Editor navigation | 10.5 | 37.8 | 9.3 | 12.5 |
| Restore | Typing | 14.6 | 14.7 | 14.6 | 15.4 |
| Restore | Restore navigation | unavailable | unavailable | 49.8 | 61.8 |
| Scan | Checkbox selection | 293.6 | 301.7 | 50.3 | 83.4 |
| Scan | Editor navigation | 57.3 | 58.9 | 50.2 | 63.4 |
| Scan | Typing | 64.5 | 105.0 | 65.1 | 118.0 |
| Scan | Restore navigation | 99.4 | 115.8 | 65.5 | 66.0 |

Baseline Restore navigation was disabled in all 50 active-restore attempts.
Afterward all 50 attempts opened the panel with an enabled, focused search
control, in every workload. Backup completed two passes during baseline
sampling; the faster after-change interaction phase ended during its first pass.

### Separate after-change two-core / memory-pressure scenario

Same disk-backed dataset and 50 samples, CPU affinity `0,1`, `MemoryHigh=384M`,
`MemoryMax=1G`, and 512 MiB retained allocation. The cgroup high counts at the end
of the four workloads were 1183, 2300, 3427, and 4770; max/OOM/OOM-kill counts
were zero. These counts are cumulative across the process, not per-row deltas.

Cells below are **p95 / worst**, in milliseconds.

| Workload | Checkbox selection | Editor navigation | Typing | Restore navigation |
| --- | ---: | ---: | ---: | ---: |
| Idle | 51.4 / 52.1 | 9.0 / 12.8 | 16.0 / 17.6 | 50.2 / 65.7 |
| Backup | 51.5 / 52.0 | 12.4 / 23.5 | 18.4 / 19.4 | 65.4 / 66.7 |
| Restore | 51.3 / 53.1 | 9.8 / 12.7 | 16.7 / 17.0 | 49.8 / 60.6 |
| Scan | 51.6 / 83.6 | 51.5 / 82.7 | 65.5 / 65.7 | 52.3 / 82.5 |

All 50 Restore openings again had useful focus in each workload. There were no
disabled navigation attempts or transfer completions at the measured boundary.

## Remaining validation

The measured Qt submission proxies are below the 100 ms feedback and 200 ms
navigation p95 budgets after the changes. They demonstrate the large-selection
improvement and navigation availability; they do not establish exact visible
latency on the specified two-core, 4 GB RAM, integrated-graphics, SATA-SSD target.

Keep #22 open for the originally proposed i5-6200U laptop run, exact target
hardware validation, actual rendered/presented feedback measurements, and
representative real Proton/systemd-worker contention with live progress state.
The faster desktop, small warm-cache payloads, and temporary CPU/memory limits
must not be presented as equivalent to that hardware or workload.

## Follow-up optimization checks

Deterministic regression checks cover the subsequent optimizations separately
from the recorded frame-submission measurements:

- `protonprovider-test`: backing up 100 files to `/backups/copy` uses 101 uploads
  (payloads plus manifest). Existing-namespace mode uses two directory listings
  and 201 metadata inspections. Fresh-copy mode with content metadata uses three
  listings (two parent checks plus one post-upload listing) and one manifest
  inspection. Storage-only metadata uses three listings and 101 inspections;
  partial metadata uses individual checks only for omitted entries. Across two
  payload folders, full metadata uses two verification listings and just the
  manifest inspection; unsupported bulk is attempted only once. Nested sibling
  directories share cached ancestor checks, with cache expiry after successful
  or failed operations. Removing an ancestor during upload forces full rechecking
  and a retry of the immutable snapshot.
- `protonprovider-test`: fresh-copy verification rejects payloads removed, truncated,
  or same-size corrupted after the last upload, using both bulk and individual
  metadata. Listing failures fall back without claiming premature success. Verified
  progress, incomplete manifests, checksums, and single-payload staging cleanup are
  checked independently of transfer command counts.
- `protonprovider-test`: verification of 100 files across two payload folders
  uses two listings and zero individual inspections when valid listing content
  metadata is available. Partial metadata needs two individual fallbacks;
  storage-only/failed listings need one bulk attempt plus 100 individual checks.
  Invalid/ambiguous/out-of-folder metadata, checksum mismatch, retention gates,
  and cancellation after a bulk call are covered separately.
- `protonclifixture-test`: empty and multi-chunk restores cover size-only and
  SHA-256 verification, truncated/same-size-corrupt transfers, and preservation
  of existing destination bytes and staging cleanup. Restore hashing and atomic
  copying now share one read; downloads into unused private staging paths are moved.
- `backupengine-test`: 100 files in one nested folder ensure only the copy root
  and payload parent. A simulated disappearing directory causes one recheck and
  retry, and every resulting entry retains its verified checksum. A deterministic
  40,000-phase stream over ten simulated seconds permits ten ordinary progress
  saves, with separate forced file-completion and finalization samples.
- `workerrecovery-test`: a blocked upload still persists its current uploading
  phase without another engine callback; termination/recovery and manifest-failure
  cases continue to pass with the serialized progress writer.
- `retention-discovery-test`: scoped discovery uses one copy-folder listing, three
  manifest downloads (including a rejected foreign identity), and two payload
  inspections. It does not list nested payload directories or other computers.
  Removing the newest copy's payload prevents that copy from evicting the older
  verified copy.
- `backupsetcontroller-test`: repeated unchanged polls do not publish run-state,
  cleanup, transfer, or dashboard resets. Progress changes notify transfer display
  independently; successful completion updates the recent-backup model. Malformed
  state retains last-good data and clears its error after recovery.
- `backuprestorecontroller-test`: switching away from a 100-file copy while its
  first inspection is blocked results in only that inspection and the newly
  selected copy's inspection, rather than completing the obsolete 100-file scan.
- Existing engine exclusions tests continue to cover hidden content, bare-name
  and absolute rules, and individual excluded-file reporting in the preview.
- `backupengine-test`: cooperative preview cancellation stops traversal and
  discards partial lists; a later uncancelled scan remains complete.
- `dashboard-test`: toggling with 10,000 selected files retains selection-array
  and lookup identity, keeps reactive counts current, and handles the swapped
  final selection correctly. Offscreen selections, refreshed path order, and
  return scroll remain covered.

The full 22-target CTest suite passes. These regression checks establish command
counts and behavior; they do not measure presentation latency or end-to-end cloud
backup speed.

## Manifest and CLI measurements, 2026-10-03

Run the opt-in manifest loader benchmark from the repository root:

```sh
cmake --build build --target manifest-benchmark
QT_FORCE_STDERR_LOGGING=1 QT_LOGGING_RULES='*.info=true' \
  TMPDIR="$PWD/build" build/manifest-benchmark
```

The benchmark loads valid incomplete version-2 manifests through the production
loader. Each fixture has equal numbers of verified entries and distinct failed
paths; its construction is outside the measured interval. Results include JSON
parsing and validation, with one warm-up followed by five samples. The environment
is the development desktop documented above, using the existing workspace build
and disk-backed temporary files; these are warm-cache loader timings.

The baseline uses the same build and benchmark with the entry-validation lookup
changed back from `failedSet.contains(restorePath)` to
`failedItems.contains(restorePath)`. Restore the set lookup and rebuild to reproduce
the after result. All other loader work is identical.

| Verified entries / failed paths | Manifest bytes | Before median | After median | Reduction |
| --- | ---: | ---: | ---: | ---: |
| 1,000 / 1,000 | 352,537 | 10.6 ms | 10.4 ms | approximately 2% |
| 10,000 / 10,000 | 3,583,537 | 152.4 ms | 107.2 ms | approximately 30% |

This is about a 1.42x loader speedup for the larger incomplete fixture, not a
prediction that every manifest or entire backup becomes 1.42x faster.

Read-only measurements against the installed Proton CLI also checked the actual
listing contract. The `/my-files` root listed five file nodes, four with content
size metadata; omitted metadata still needs the individual fallback. After an
initial probe, three root listings had a median of 807 ms, and three inspections
of one file had a median of 756 ms. Initial probes were slower (approximately
1.4 seconds for root info and 2.9 seconds for listing), showing startup/network
variation. These small samples are not a representative payload-directory or
end-to-end backup benchmark. No cloud uploads or deletions were performed.

The deterministic fresh-copy fixture reduces verification of 100 payloads in one
folder from 100 individual inspections to one listing when complete content
metadata is available: 99% fewer payload-verification requests. Uploads and the
manifest's individual verification remain. In the storage-only case, there is one
extra attempted listing before the original individual checks; no speedup is
claimed for that fallback.

## Restore selection extraction — issue #36, 2026-10-04

Compared a detached worktree at `d3dcadc` with the selection-owner extraction on
that revision. Both used the same desktop hardware/environment described above,
Qt 6.11.2 / GCC 16.2.1, Wayland/OpenGL (graphics API 3), Fusion controls, and
disk-backed fixtures with `TMPDIR` set to the workspace `build` directory
(Btrfs/NVMe). Both CMake builds used the default empty build type and C++ flags.
There was no CPU restriction or memory-pressure allocation; cgroup high/OOM
counts were zero. Each run completed all four workloads with 50 samples each.

The existing workload and timing boundary are unchanged: an actual checkbox click
with 100 ticks in a 10,000-file model, timed to the next Qt `frameSwapped`.
Only selection setup changed to the coherent `setRestoreSelection(indexes)`
operation, outside the measured interval, instead of two property assignments.
The 10,000-tick/offscreen correctness workload remains in `dashboard-test`.

| Workload | Before p95 | Before worst | After p95 | After worst |
| --- | ---: | ---: | ---: | ---: |
| Idle | 50.2 ms | 50.2 ms | 50.2 ms | 50.2 ms |
| Backup | 50.1 ms | 50.2 ms | 50.1 ms | 50.3 ms |
| Restore | 50.3 ms | 50.8 ms | 50.1 ms | 50.2 ms |
| Scan | 50.4 ms | 66.8 ms | 50.3 ms | 50.8 ms |

No material selection regression was observed; p95 remains approximately 50 ms
in every workload. The 0.2 ms increase in Backup worst-case timing is within the
frame-submission timing variation. Neither run had selection/completion boundary
crossings or disabled restore navigation, and all 50 navigation samples per
workload had useful focus. These remain Qt submission proxies rather than
physical presentation or target-laptop validation for #22.

The native build and all 24 CTest targets pass. Dashboard coverage additionally
checks coherent array notifications, invalid/duplicate bulk indexes, independent
default/customized/cleared context snapshots, selection becoming empty after
removal without reselecting unrelated files, and completion resetting selection
metadata and the cached context.
