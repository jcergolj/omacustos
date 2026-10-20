#pragma once

#include "backupprovider.h"
#include "incrementalplanner.h"
#include <QJsonObject>

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
    int retention = 0; // Zero disables cleanup; positive values keep newest N saved states.
    QString legacyRoot; // Optional exact legacy set folder; never a shared repository.
    QString cleanupConfirmation; // SHA-256 of the exact presented proposal bytes.
    qint64 integrityByteBudget = 10LL * 1024 * 1024 * 1024;
    qint64 integrityTimeBudgetMs = 60LL * 60 * 1000;
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
    bool cleanupNeedsAttention = false;
    QJsonObject cleanupProposal;
    QString cleanupConfirmation;
    QJsonObject integrity;
    bool success() const { return state == State::Saved || state == State::Unchanged; }
};

// CLI-compatible, operational single-writer repository. No remote lease/fencing
// claim: the owner must disable all old writers before an offline handoff.
class IncrementalRepository
{
public:
    static bool initialize(const IncrementalWriteRequest &request, BackupProvider &provider,
        bool singleWriterConfirmed, QString *error);
    static IncrementalWriteResult execute(const IncrementalWriteRequest &request, BackupProvider &provider);
    // Advances a due cycle, or starts one explicitly. No archive/history mutation.
    static QJsonObject checkIntegrity(const IncrementalWriteRequest &request, BackupProvider &provider,
        bool force = false);
    // Also supports retry after a saved backup, without re-running the writer.
    static bool cleanup(const IncrementalWriteRequest &request, BackupProvider &provider,
        QJsonObject *proposal, QString *confirmation, QString *error);
    // Request identifies the exact CURRENT anchor. Only a secured offline writer
    // handoff is supported; never an online lease steal or server-fencing claim.
    static bool takeover(const IncrementalWriteRequest &current, BackupProvider &provider,
        const QString &newComputerId, bool oldWriterPermanentlyDisabled, QString *error);
};
