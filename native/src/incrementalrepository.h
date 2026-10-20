#pragma once

#include "backupprovider.h"
#include "incrementalplanner.h"

enum class IncrementalControl { Continue, Pause, Cancel };

struct IncrementalWriteRequest {
    QString remoteSetRoot; // Exact .../sets/<set UUID>, never a legacy copy folder.
    QString computerId;
    QString originComputerId;
    qint64 ownershipEpoch = 1;
    QString operationId;
    QString snapshotId;
    IncrementalPlanRequest scan;
    QString stateDirectory; // Empty uses the shared application state directory.
    QString stagingDirectory;
    qint64 stagingBudget = 1000 * 1000 * 1000;
    qint64 archiveTargetBytes = 1000 * 1000 * 1000;
    int batchFileLimit = 1000;
    // Called at durable boundaries; capture/packing also poll the current control.
    std::function<IncrementalControl(const QString &)> checkpoint;
};

struct IncrementalWriteResult {
    enum class State { Failed, Retrying, Waiting, Paused, Cancelled, Saved, Unchanged };
    State state = State::Failed;
    bool manifestCommitted = false;
    QString manifestPath;
    QDateTime lastSuccessfulCheck;
    QDateTime lastSavedChange;
    QStringList affectedSnapshots;
    QStringList damagedObjects;
    QVector<BackupIssue> issues;
    bool success() const { return state == State::Saved || state == State::Unchanged; }
};

// CLI-compatible, operational single-writer repository. No remote lease/fencing
// claim: the owner must disable all old writers before an offline handoff.
// The executor never invokes retention, trash, or permanent deletion.
class IncrementalRepository
{
public:
    static bool initialize(const IncrementalWriteRequest &request, BackupProvider &provider,
        bool singleWriterConfirmed, QString *error);
    static IncrementalWriteResult execute(const IncrementalWriteRequest &request, BackupProvider &provider);
};
