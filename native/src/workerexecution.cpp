#include "backupengine.h"
#include "backupconfig.h"
#include "backuprunstore.h"
#include "backupprerequisites.h"
#include "backupcatalog.h"
#include "backupstaging.h"
#include "backupcontinuation.h"
#include "backupecleanup.h"
#include "protonprovider.h"
#include "protonfolderlink.h"
#include "qprocessrunner.h"
#include "workerexecution.h"
#include "incrementalworker.h"
#include "incrementalrestore.h"

#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
#include <QSet>
#include <QSysInfo>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

// Keep the latest display sample while synchronous CLI transfers are running.
// Only this writer touches run-state persistence during the engine operation;
// it is joined before the worker publishes its durable success/failure status.
class ProgressWriter final
{
public:
    ProgressWriter(const QString &path, const QElapsedTimer &clock)
        : pending(path), clock(clock), thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            while (!stopping) {
                wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping; });
                if (dirty && persistence.shouldSave(progress, this->clock.elapsed())) save();
            }
            if (dirty) save();
        })
    {
    }

    ~ProgressWriter()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        thread.join();
    }

    void update(const BackupRunRecord &record, const BackupProgress &sample)
    {
        std::lock_guard<std::mutex> lock(mutex);
        attempt = record;
        progress = sample;
        dirty = true;
        if (persistence.shouldSave(progress, clock.elapsed())) save();
    }

private:
    void save()
    {
        QString error;
        dirty = !pending.publishProgress(attempt, &error);
        if (dirty) {
            qWarning().noquote() << QStringLiteral("Unable to update backup progress:") << error;
        }
    }

    BackupRunStore pending;
    BackupRunRecord attempt;
    const QElapsedTimer &clock;
    BackupProgress progress;
    BackupProgressPersistence persistence;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool dirty = false;
    std::thread thread;
};

QString copyId()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz"))
        + QStringLiteral("-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

}

int runBackupWorker(const QString &configuredPath, BackupPrerequisiteProbe &prerequisites)
{
    const QString configPath = QFileInfo(configuredPath).absoluteFilePath();
    const QString stateDirectory = QFileInfo(configPath).absolutePath();
    // Management can accept enqueue while holding worker exclusion. Wait for
    // it before trying exclusion, so that service invocation is not lost.
    QLockFile managementGate(QDir(stateDirectory).filePath(QStringLiteral("omacustos-backup-runs.json.management.lock")));
    managementGate.setStaleLockTime(0);
    if (!managementGate.lock()) {
        qCritical() << "Unable to coordinate with backup copy management.";
        return 1;
    }
    QLockFile processLock(configPath + QStringLiteral(".worker.lock"));
    processLock.setStaleLockTime(0);
    if (!processLock.tryLock(0)) {
        return 0;
    }
    // Different config paths in the same directory still share one run queue.
    QLockFile queueWorkerLock(QDir(stateDirectory).filePath(QStringLiteral("omacustos-backup-runs.json.worker.lock")));
    queueWorkerLock.setStaleLockTime(0);
    if (!queueWorkerLock.tryLock(0)) {
        return 0;
    }
    managementGate.unlock();

    BackupConfig config;
    QString error;
    if (!BackupConfigStore(configPath).load(&config, &error)) {
        qCritical().noquote() << error;

        return 1;
    }

    QProcessRunner runner(config.protonBinary);
    ProtonProvider provider(runner);
    BackupEngine engine;
    const QString cleanupPath = QDir(stateDirectory).filePath(QStringLiteral("omacustos-backup-cleanup.json"));
    CleanupStore cleanupStore(cleanupPath);
    if (!cleanupStore.load(&error)) {
        qCritical().noquote() << error;
        return 1;
    }
    BackupRunStore runStore(QDir(stateDirectory).filePath(QStringLiteral("omacustos-backup-runs.json")));
    if (!runStore.load(&error)) { qCritical().noquote() << error; return 1; }
    QStringList selectedSources;
    for (const auto &set : config.sets) selectedSources.append(set.sourceDirectories);
    QSet<QString> stagingRoots;
    for (const auto &record : runStore.records())
        for (const auto &root : record.stagingRoots) stagingRoots.insert(root);
    // Recover staging even when a durable pause/cancel means no engine will run.
    // Historical roots include disks selected before an interruption/settings edit.
    for (const auto &root : stagingRoots) {
        BackupStaging recovery;
        QString recoveryError;
        recovery.open(root, selectedSources, &recoveryError);
    }
    if (!runStore.prepareWorker(config.sets, QDateTime::currentDateTime(), &error)) {
        qCritical().noquote() << error;

        return 1;
    }

    QSet<QString> attempted;
    while (true) {
        if (!runStore.load(&error)) {
            qCritical().noquote() << error;
            return 1;
        }
        const QDateTime now = QDateTime::currentDateTime();
        QString setId;
        for (int index : runStore.readyIndexes(now)) {
            const QString candidate = runStore.records().at(index).setId;
            if (!attempted.contains(candidate)) {
                setId = candidate;
                break;
            }
        }
        if (setId.isEmpty()) break;
        attempted.insert(setId);
        const auto setIterator = std::find_if(config.sets.cbegin(), config.sets.cend(), [&setId](const BackupSet &set) {
            return set.id == setId;
        });
        if (setIterator == config.sets.cend()) {
            continue;
        }

        const BackupPrerequisiteResult prerequisite = BackupPrerequisites::check(*setIterator, prerequisites);
        if (!prerequisite.ready) {
            if (!runStore.waitForPrerequisite(setId, prerequisite.reason, now, &error)) {
                qCritical().noquote() << error;
                return 1;
            }
            continue;
        }

        QString manifestPath;
        const QString computerName = QSysInfo::machineHostName();
        const QString copy = copyId();
        const bool newDefault = qEnvironmentVariable("OMACUSTOS_INTERNAL_LEGACY_WORKER") != "1";
        const QString proposedRoot = newDefault
            ? IncrementalWorker::root(*setIterator) + "/snapshots/" + QUuid::createUuid().toString(QUuid::WithoutBraces)
            : QDir(setIterator->remoteFolder(computerName)).filePath(copy);
        BackupRunRecord record;
        if (!runStore.beginAttempt(setId, proposedRoot, &record, &error)) {
            qCritical().noquote() << error;
            return 1;
        }
        const QString copyRoot = record.remoteCopyPath;
        QString sharedRoot, sharedSetId, snapshotId;
        const bool incremental = IncrementalRestore::snapshotLocation(copyRoot, &sharedRoot, &sharedSetId, &snapshotId);
        error.clear();
        const BackupCopyMetadata metadata {
            computerName,
            setIterator->id,
            setIterator->name,
            QFileInfo(copyRoot).fileName(),
            QDateTime::currentDateTimeUtc(),
        };
        QElapsedTimer progressClock;
        progressClock.start();
        QElapsedTimer controlClock;
        controlClock.start();
        bool stop = false;
        const auto stopped = [&] {
            if (stop) return true;
            if (controlClock.elapsed() < 100) return false;
            controlClock.restart();
            BackupRunStore controls(runStore.filePath());
            QString controlError;
            if (!controls.load(&controlError)) { stop = true; return true; }
            const auto *current = controls.find(setId);
            stop = !current || current->remoteCopyPath != copyRoot || current->attempts != record.attempts
                || current->status != "running" || !current->controlRequest.isEmpty();
            return stop;
        };
        runner.setStopRequested(stopped);
        BackupOptions options;
        options.freshCopy = true;
        options.singleArchive = qEnvironmentVariable("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE") == "1";
        // Fresh manual and scheduled copies share this default. Explicit zero
        // is retained only for historical-format integration fixtures.
        options.boundedArchives = !options.singleArchive
            && qEnvironmentVariable("OMACUSTOS_INTERNAL_BOUNDED_ARCHIVES") != "0";
        bool archiveTargetValid = false;
        const qint64 archiveTarget = qEnvironmentVariable("OMACUSTOS_INTERNAL_ARCHIVE_TARGET").toLongLong(&archiveTargetValid);
        if (archiveTargetValid && archiveTarget > 0) options.archiveTargetBytes = archiveTarget;
        options.stagingDirectory = setIterator->stagingDirectory;
        options.stagingBudget = setIterator->stagingBudget;
        options.inclusions = setIterator->inclusions;
        options.continuationDirectory = QDir(stateDirectory).filePath(QStringLiteral("continuations/")
            + QString::fromLatin1(QCryptographicHash::hash(copyRoot.toUtf8(), QCryptographicHash::Sha256).toHex()));
        const int recordedFormat = BackupContinuation::recordedFormat(options.continuationDirectory, &error);
        if (recordedFormat < 0) {
            qCritical().noquote() << error;
            return 1;
        }
        if (recordedFormat != 0) {
            // Recovery follows the durable copy format, not today's defaults or
            // the environment used to start the replacement worker.
            options.singleArchive = false;
            options.boundedArchives = recordedFormat == 3;
        }
        options.stopped = stopped;
        options.retainLocalManifest = false;
        options.stagingReady = [&](const QString &root, QString *error) { return runStore.rememberStagingRoot(record, root, error); };
        bool succeeded;
        if (!incremental && setIterator->readOnly) {
            succeeded = false;
            record.result.reported = record.result.interrupted = true;
            error = QStringLiteral("This copied, imported or unbound configuration is read-only. Explicit offline writer enrollment/takeover is required before resuming historical work.");
        } else if (incremental) {
            IncrementalWriteRequest request;
            succeeded = false;
            record.result.reported = true;
            if (sharedRoot != IncrementalWorker::root(*setIterator)) {
                error = QStringLiteral("The unfinished incremental run belongs to another remote set. Resume it with its original configuration.");
                record.result.interrupted = true;
            } else if (!IncrementalWorker::request(*setIterator, stateDirectory, snapshotId, provider, &request, &error, true,
                [&](const QString &root, QString *error) { return runStore.rememberStagingRoot(record, root, error); })) {
                record.result.interrupted = true;
            } else {
                request.checkpoint = [&](const QString &) { return stopped() ? IncrementalControl::Pause : IncrementalControl::Continue; };
                if (!IncrementalRepository::recoverCleanup(request, provider, &error)) record.result.interrupted = true;
                request.checkpoint = {};
                // Reconcile cancellations made while the worker was stopped,
                // before beginning any replacement operation. No object deletion.
                for (const auto &cancelled : record.cancelledCopies) {
                    if (!error.isEmpty()) break;
                    QString oldRoot, oldSet, oldSnapshot;
                    if (!IncrementalRestore::snapshotLocation(cancelled, &oldRoot, &oldSet, &oldSnapshot) || oldRoot != sharedRoot) continue;
                    auto old = request;
                    old.operationId = old.snapshotId = oldSnapshot;
                    old.retention = 0;
                    runner.setStopRequested({});
                    const auto reconciled = IncrementalWorker::run(*setIterator, old, provider,
                        [](const QString &) { return IncrementalControl::Cancel; });
                    if (!reconciled.success() && reconciled.state != IncrementalWriteResult::State::Cancelled) {
                        error = QStringLiteral("Unable to reconcile the cancelled protected operation: ") + oldSnapshot;
                        break;
                    }
                }
                if (error.isEmpty()) {
                    ProgressWriter progressWriter(runStore.filePath(), progressClock);
                    IncrementalControl requested = IncrementalControl::Continue;
                    const auto control = [&](const QString &phase) {
                        if (requested == IncrementalControl::Continue && stopped()) {
                            BackupRunStore controls(runStore.filePath());
                            QString controlError;
                            if (controls.load(&controlError)) {
                                const auto *current = controls.find(setId);
                                requested = current && current->controlRequest == "cancel"
                                    ? IncrementalControl::Cancel : IncrementalControl::Pause;
                            } else requested = IncrementalControl::Pause;
                            // Once stopping, allow the repository to persist its
                            // cancellation/reconciliation records after a killed transfer.
                            runner.setStopRequested({});
                        }
                        record.progress.phase = phase;
                        record.progressElapsedMs = progressClock.elapsed();
                        record.progressUpdatedAt = QDateTime::currentDateTimeUtc();
                        progressWriter.update(record, record.progress);
                        return requested;
                    };
                    runner.setStopRequested(stopped);
                    const auto capacityWarning = [&](const QString &message) {
                        record.incremental["capacity_warning"] = message;
                        qInfo().noquote() << setIterator->name << message;
                        progressWriter.update(record, record.progress);
                    };
                    auto outcome = IncrementalWorker::run(*setIterator, request, provider, control, capacityWarning, configPath);
                    if (requested == IncrementalControl::Cancel && !outcome.success()
                        && outcome.state != IncrementalWriteResult::State::Cancelled)
                        outcome = IncrementalWorker::run(*setIterator, request, provider, control, capacityWarning, configPath);
                    if (!outcome.capacityWarning.isEmpty()) record.incremental["capacity_warning"] = outcome.capacityWarning;
                    succeeded = outcome.success();
                    record.result.manifestVerified = succeeded;
                    record.result.verifiedFiles = outcome.savedFiles;
                    record.result.verifiedBytes = outcome.savedBytes;
                    record.result.issues = outcome.issues;
                    record.result.waitingForSpace = outcome.state == IncrementalWriteResult::State::Waiting;
                    record.result.interrupted = !succeeded && outcome.state != IncrementalWriteResult::State::Cancelled;
                    record.incremental["outcome"] = outcome.state == IncrementalWriteResult::State::Saved ? "saved"
                        : outcome.state == IncrementalWriteResult::State::Unchanged ? "unchanged" : "unfinished";
                    if (!outcome.latestSnapshotPath.isEmpty()) record.incremental["snapshot_path"] = outcome.latestSnapshotPath;
                    if (outcome.lastSuccessfulCheck.isValid()) record.incremental["last_successful_check"] = outcome.lastSuccessfulCheck.toString(Qt::ISODateWithMs);
                    if (outcome.lastSavedChange.isValid()) record.incremental["last_saved_change"] = outcome.lastSavedChange.toString(Qt::ISODateWithMs);
                    record.incremental["cleanup_needs_attention"] = outcome.cleanupNeedsAttention;
                    record.incremental["integrity"] = outcome.integrity;
                    record.incremental["affected_snapshots"] = QJsonArray::fromStringList(outcome.affectedSnapshots);
                    record.incremental["damaged_objects"] = QJsonArray::fromStringList(outcome.damagedObjects);
                    record.incremental["cleanup_proposal"] = outcome.cleanupProposal;
                    record.incremental["cleanup_confirmation"] = outcome.cleanupConfirmation;
                    manifestPath = outcome.manifestPath;
                    if (!succeeded) for (const auto &issue : outcome.issues) error += issue.reason + ' ';
                }
            }
        } else {
            ProgressWriter progressWriter(runStore.filePath(), progressClock);
            const auto reportProgress = [&](const BackupProgress &progress) {
                record.progress = progress;
                record.progressElapsedMs = progressClock.elapsed();
                record.progressUpdatedAt = QDateTime::currentDateTimeUtc();
                progressWriter.update(record, progress);
            };
            succeeded = engine.backup(setIterator->sourceDirectories, copyRoot, setIterator->exclusions,
                metadata, provider, &manifestPath, &error, reportProgress, &record.result, options);
        }
        runner.setStopRequested({});
        if (!incremental && stopped()) succeeded = false;
        if (!incremental && succeeded) {
            record.incremental = {{"outcome", "legacy-saved"}, {"snapshot_path", copyRoot},
                {"cleanup_needs_attention", true}};
            // Resume the durable format, not its old deletion authorization.
            // Retention is deferred until a fresh baseline permits an exact
            // mixed-format proposal; no legacy history is deleted here.
        }
        if (succeeded) {
            record.progressElapsedMs = progressClock.elapsed();
            qInfo().noquote() << setIterator->name << manifestPath;
        } else {
            qCritical().noquote() << setIterator->name << error << manifestPath;
        }

        if (!incremental && record.result.manifestVerified) {
            // Prepare navigation while this copy is being finalized. Link
            // lookup/cache failures must not change the backup's result.
            ProtonFolderLink::resolve(runner, copyRoot, ProtonFolderLink::cachePath(configPath));
        }

        QString persistenceError;
        if (!runStore.completeAttempt(record, succeeded, error, QDateTime::currentDateTime(), &persistenceError)) {
            qCritical().noquote() << persistenceError;

            return 1;
        }
        // A final manifest is the durable outcome; checkpoints for terminal work
        // are no longer needed. Interrupted/paused work keeps its journal.
        const auto *completed = runStore.find(setId);
        if (!incremental && completed && !completed->unfinished) QDir(options.continuationDirectory).removeRecursively();
        if (!incremental && !manifestPath.isEmpty()) QDir(QFileInfo(manifestPath).absolutePath()).removeRecursively();
    }

    return 0;
}
