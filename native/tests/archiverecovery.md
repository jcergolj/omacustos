# Archive interruption recovery (#50)

The archive execution path remains internally enabled with
`OMACUSTOS_INTERNAL_BOUNDED_ARCHIVES=1`. Fresh production defaults are the later
#52 slice. Recovery reads the durable `payload_format`, so a replacement worker
does not require the original opt-in environment and never mixes formats.

## Deterministic integration coverage

`workerrecovery-test::archiveInterruptionRecovery` runs the real worker against
the isolated CLI subprocess fixture. It terminates workers at source preparation,
compression, transfer, remote verification, verified checkpoint publication, and
final-index transfer. Checksum and size-only providers are covered independently.
Additional rows exercise multi-member archives, Pause, Cancel, transient and
authentication failures, checkpoint quota exhaustion, and archive staging quota
exhaustion followed by Resume on another staging disk.

Assertions cover the exact copy namespace, prepared versus durable verified counts,
logical file/byte totals, recorded-format recovery, staging cleanup, no redundant
uploads of unchanged verified groups, scheduler respect for Pause, terminal Cancel
history, and no retention operations. Each resumed copy is restored through the
public restore controller and compared byte-for-byte with its expected contents.

`archiveReuseStorageWaiting` covers a reduced staging budget and actual download
quota failure despite available filesystem space. Both use size-only metadata and
two-member groups; recovery must enter Waiting without re-uploading the verified
archives, and later complete after budget/quota recovery or a staging-disk change.

`backupengine-test::archiveControlsPreserveOnlyDurableProgress` requests cooperative
stops at preparation, upload, verification, checkpointing, and finalization report
boundaries. It checks journal state and prevents late manifest success, then resumes.
Existing run-store tests cover queue mutations and progress/outcome serialization;
existing process-runner tests cover stopping owned CLI process groups.

```sh
cmake --build build -j 4
./build/backupengine-test archiveControlsPreserveOnlyDurableProgress
./build/workerrecovery-test archiveInterruptionRecovery archiveReuseStorageWaiting
ctest --test-dir build --output-on-failure -j 4
```

These are local correctness fixtures, not measurements of Proton throughput.
Prepared-upload reconciliation and changes to selected sources or remote archives
are the following #51 slice; unverified prepared groups are safely retried here.
