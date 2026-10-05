#include "backupengine.h"
#include "backupconfig.h"
#include "backuprunstore.h"
#include "backupprerequisites.h"
#include "backupschedule.h"
#include "backupcatalog.h"
#include "backupecleanup.h"
#include "protonprovider.h"
#include "protonfolderlink.h"
#include "qprocessrunner.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
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

    void update(const BackupRunStore &store, const BackupProgress &sample)
    {
        BackupRunStore snapshot = store;
        // The worker retains a reference to its running record, so do not share
        // the outer record vector with that reference across threads.
        snapshot.records().detach();
        std::lock_guard<std::mutex> lock(mutex);
        pending = std::move(snapshot);
        progress = sample;
        dirty = true;
        if (persistence.shouldSave(progress, clock.elapsed())) save();
    }

private:
    void save()
    {
        QString error;
        dirty = !pending.save(&error);
        if (dirty) {
            qWarning().noquote() << QStringLiteral("Unable to update backup progress:") << error;
        }
    }

    BackupRunStore pending;
    const QElapsedTimer &clock;
    BackupProgress progress;
    BackupProgressPersistence persistence;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool dirty = false;
    std::thread thread;
};

bool authenticationFailure(const QString &error)
{
    const QString message = error.toLower();
    return message.contains(QStringLiteral("auth"))
        || message.contains(QStringLiteral("sign in"))
        || message.contains(QStringLiteral("login"))
        || message.contains(QStringLiteral("401"));
}

QString copyId()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz"))
        + QStringLiteral("-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString setRemoteSegment(const BackupSet &set, const QString &computerName)
{
    return set.remoteFolder(computerName);
}

}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({{"c", "config"}, QStringLiteral("Configuration file."), QStringLiteral("path")});
    parser.process(application);

    const QString configuredPath = parser.value(QStringLiteral("config")).isEmpty()
        ? QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup.json"))
        : parser.value(QStringLiteral("config"));
    const QString configPath = QFileInfo(configuredPath).absoluteFilePath();
    QLockFile processLock(configPath + QStringLiteral(".worker.lock"));
    processLock.setStaleLockTime(0);
    if (!processLock.tryLock(0)) {
        return 0;
    }
    const QString stateDirectory = QFileInfo(configPath).absolutePath();
    QLockFile runStateLock(QDir(stateDirectory).filePath(QStringLiteral("omacustos-backup-runs.json.lock")));
    runStateLock.setStaleLockTime(0);
    if (!runStateLock.tryLock(0)) {
        return 0;
    }

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
    if (!runStore.load(&error)) {
        qCritical().noquote() << error;

        return 1;
    }
    if (runStore.recoverInterrupted(QDateTime::currentDateTime()) && !runStore.save(&error)) {
        qCritical().noquote() << error;
        return 1;
    }
    for (const BackupSet &set : config.sets) {
        runStore.ensureSet(set.id);
        BackupRunRecord *record = runStore.find(set.id);
        const QDateTime now = QDateTime::currentDateTime();
        if (set.schedule.enabled()) {
            QDateTime due = record->nextScheduled;
            if (record->lastScheduled.isValid()) {
                const QDateTime recalculated = BackupScheduleCalculator::nextRun(set.schedule, record->lastScheduled);
                if (recalculated.isValid() && recalculated != due) {
                    due = recalculated;
                    record->nextScheduled = recalculated;
                }
            }
            if (!due.isValid()) {
                const QDateTime currentDue = BackupScheduleCalculator::dueRun(set.schedule, now);
                if (currentDue.isValid() && currentDue <= now) {
                    due = currentDue;
                } else {
                    record->nextScheduled = BackupScheduleCalculator::nextRun(set.schedule, now);
                }
            }
            if (due.isValid() && due <= now) {
                record->lastScheduled = due;
                record->nextScheduled = BackupScheduleCalculator::nextRun(set.schedule, now);
                runStore.enqueue(set.id, QStringLiteral("schedule"), due);
            }
        }
    }
    if (!runStore.save(&error)) {
        qCritical().noquote() << error;

        return 1;
    }

    SystemBackupPrerequisiteProbe prerequisites;
    const QDateTime now = QDateTime::currentDateTime();
    for (const int index : runStore.readyIndexes(now)) {
        BackupRunRecord &record = runStore.records()[index];
        const auto setIterator = std::find_if(config.sets.cbegin(), config.sets.cend(), [&record](const BackupSet &set) {
            return set.id == record.setId;
        });
        if (setIterator == config.sets.cend()) {
            continue;
        }

        const BackupPrerequisiteResult prerequisite = BackupPrerequisites::check(*setIterator, prerequisites);
        if (!prerequisite.ready) {
            runStore.markWaiting(record, prerequisite.reason, now);
            continue;
        }

        runStore.markRunning(record);
        error.clear();
        if (!runStore.save(&error)) {
            qCritical().noquote() << error;

            return 1;
        }
        QString manifestPath;
        const QString computerName = QSysInfo::machineHostName();
        const QString copy = copyId();
        const QString copyRoot = QDir(setIterator->remoteFolder(computerName)).filePath(copy);
        record.remoteCopyPath = copyRoot;
        const BackupCopyMetadata metadata {
            computerName,
            setIterator->id,
            setIterator->name,
            copy,
            QDateTime::currentDateTimeUtc(),
        };
        QElapsedTimer progressClock;
        progressClock.start();
        bool succeeded;
        {
            ProgressWriter progressWriter(runStore.filePath(), progressClock);
            const auto reportProgress = [&](const BackupProgress &progress) {
                record.progress = progress;
                record.progressElapsedMs = progressClock.elapsed();
                record.progressUpdatedAt = QDateTime::currentDateTimeUtc();
                progressWriter.update(runStore, progress);
            };
            succeeded = engine.backup(setIterator->sourceDirectories, copyRoot, setIterator->exclusions,
                metadata, provider, &manifestPath, &error, reportProgress, &record.result, {true});
        }
        if (succeeded) {
            record.progressElapsedMs = progressClock.elapsed();
            runStore.markSuccess(record, QDateTime::currentDateTime());
            qInfo().noquote() << setIterator->name << manifestPath;

            QVector<RemoteCopy> copies;
            QString catalogError;
            if (BackupCatalog::discoverCopies(provider, setIterator->remoteFolder(computerName),
                    setIterator->id, &copies, &catalogError)) {
                const QStringList targets = BackupCleanup::eligibleTargets(
                    copies, setIterator->retention, computerName, setIterator->id);
                if (!BackupCleanup::run(provider, cleanupStore, setIterator->id, targets,
                        setIterator->remoteFolder(computerName), &catalogError)) {
                    qCritical().noquote() << setIterator->name << QStringLiteral("Cleanup failed:") << catalogError;
                }
            } else {
                qCritical().noquote() << setIterator->name << QStringLiteral("Cleanup preview unavailable:") << catalogError;
            }
        } else if (authenticationFailure(error)) {
            runStore.markAuthenticationRequired(record, error, QDateTime::currentDateTime());
            qCritical().noquote() << setIterator->name << error;
        } else if (record.result.manifestVerified && record.result.verifiedFiles > 0) {
            runStore.markIncomplete(record, error, QDateTime::currentDateTime());
            qCritical().noquote() << setIterator->name << error << manifestPath;
        } else {
            runStore.markFailed(record, error, QDateTime::currentDateTime());
            qCritical().noquote() << setIterator->name << error;
        }

        if (record.result.manifestVerified) {
            // Prepare navigation while this copy is being finalized. Link
            // lookup/cache failures must not change the backup's result.
            ProtonFolderLink::resolve(runner, copyRoot, ProtonFolderLink::cachePath(configPath));
        }

        if (!runStore.save(&error)) {
            qCritical().noquote() << error;

            return 1;
        }
    }

    return 0;
}
