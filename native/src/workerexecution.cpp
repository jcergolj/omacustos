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
        const QString proposedRoot = QDir(setIterator->remoteFolder(computerName)).filePath(copy);
        BackupRunRecord record;
        if (!runStore.beginAttempt(setId, proposedRoot, &record, &error)) {
            qCritical().noquote() << error;
            return 1;
        }
        const QString copyRoot = record.remoteCopyPath;
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
        {
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
        if (stopped()) succeeded = false;
        if (succeeded) {
            record.progressElapsedMs = progressClock.elapsed();
            qInfo().noquote() << setIterator->name << manifestPath;
        } else {
            qCritical().noquote() << setIterator->name << error << manifestPath;
        }

        if (record.result.manifestVerified) {
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
        if (completed && completed->status == QStringLiteral("success") && completed->result.manifestVerified) {
            // Commit success under the same lock as control decisions before
            // retention. A late pause/cancel can never authorize old-copy deletion.
            QVector<RemoteCopy> copies;
            QString catalogError;
            const QString copyParent = QFileInfo(copyRoot).path();
            if (BackupCatalog::discoverCopies(provider, copyParent, setIterator->id, &copies, &catalogError)) {
                const QStringList targets = BackupCleanup::eligibleTargets(copies, setIterator->retention, computerName, setIterator->id);
                if (!BackupCleanup::run(provider, cleanupStore, setIterator->id, targets, copyParent, &catalogError))
                    qCritical().noquote() << setIterator->name << QStringLiteral("Cleanup failed:") << catalogError;
            } else qCritical().noquote() << setIterator->name << QStringLiteral("Cleanup preview unavailable:") << catalogError;
        }
        if (completed && !completed->unfinished) QDir(options.continuationDirectory).removeRecursively();
        if (!manifestPath.isEmpty()) QDir(QFileInfo(manifestPath).absolutePath()).removeRecursively();
    }

    return 0;
}
