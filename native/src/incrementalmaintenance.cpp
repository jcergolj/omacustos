#include "incrementalmaintenance.h"
#include "incrementalworker.h"
#include "protonprovider.h"
#include "qprocessrunner.h"
#include "backupcatalog.h"
#include "backuprunstore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>

IncrementalMaintenance::IncrementalMaintenance(QString path, QObject *parent)
    : QObject(parent), configPath(std::move(path))
{
    connect(&watcher, &QFutureWatcher<QJsonObject>::finished, this, [this] {
        const auto result = watcher.result();
        working = false;
        emit busyChanged();
        if (!result.value("error").toString().isEmpty()) {
            pendingSet.clear(); pendingConfirmation.clear();
            emit failed(result.value("error").toString());
        } else if (result.contains("proposal")) {
            pendingSet = result.value("set_id").toString();
            pendingConfirmation = result.value("confirmation").toString();
            emit cleanupReady(pendingSet, QString::fromUtf8(QJsonDocument(result.value("proposal").toObject()).toJson()));
        } else {
            pendingSet.clear(); pendingConfirmation.clear();
            emit completed(result.value("message").toString());
        }
    });
}
IncrementalMaintenance::~IncrementalMaintenance() { stopped->store(true); watcher.waitForFinished(); }
void IncrementalMaintenance::previewCleanup(const QString &setId)
{
    if (!working && pendingSet.isEmpty()) start(setId, Action::Preview);
}
void IncrementalMaintenance::confirmCleanup()
{
    if (!working && !pendingSet.isEmpty() && !pendingConfirmation.isEmpty()) start(pendingSet, Action::Confirm, pendingConfirmation);
}
void IncrementalMaintenance::cancelCleanup()
{
    if (!working && !pendingSet.isEmpty() && !pendingConfirmation.isEmpty())
        start(pendingSet, Action::Dismiss, pendingConfirmation);
}
void IncrementalMaintenance::takeover(const QString &setId, bool disabled) { if (!working) start(setId, Action::Takeover, {}, disabled); }
void IncrementalMaintenance::checkIntegrity(const QString &setId) { if (!working) start(setId, Action::Integrity); }

void IncrementalMaintenance::start(const QString &setId, Action action, const QString &confirmation, bool oldWriterDisabled)
{
    working = true;
    stopped->store(false);
    emit busyChanged();
    const auto path = configPath;
    const auto stop = stopped;
    watcher.setFuture(QtConcurrent::run([path, setId, action, oldWriterDisabled, confirmation, stop] {
        const auto failure = [](const QString &error) { return QJsonObject{{"error", error}}; };
        const QString directory = QFileInfo(path).absolutePath();
        const QString runPath = QDir(directory).filePath("omacustos-backup-runs.json");
        QLockFile management(runPath + ".management.lock"), worker(path + ".worker.lock"), queue(runPath + ".worker.lock");
        management.setStaleLockTime(0); worker.setStaleLockTime(0); queue.setStaleLockTime(0);
        if (!management.tryLock(0) || !worker.tryLock(0) || !queue.tryLock(0)) return failure("Wait for the worker and queue update to finish before repository maintenance.");
        BackupConfig config;
        QString error;
        if (!BackupConfigStore(path).load(&config, &error)) return failure(error);
        auto set = std::find_if(config.sets.begin(), config.sets.end(), [&](const auto &value) { return value.id == setId; });
        if (set == config.sets.end()) return failure("The backup set no longer exists.");
        QProcessRunner runner(config.protonBinary);
        runner.setStopRequested([stop] { return stop->load(); });
        ProtonProvider provider(runner);
        if (action == Action::Takeover) {
            if (!IncrementalWorker::takeover(*set, directory, provider, oldWriterDisabled, &error)) return failure(error);
            set->readOnly = false;
            if (!BackupConfigStore(path).save(config, &error)) return failure(error);
            return QJsonObject{{"message", "Offline writer authority verified. No backup was queued."}};
        }
        IncrementalWriteRequest request;
        if (!IncrementalWorker::request(*set, directory, QUuid::createUuid().toString(QUuid::WithoutBraces), provider, &request, &error, false)) return failure(error);
        request.checkpoint = [stop](const QString &) { return stop->load() ? IncrementalControl::Pause : IncrementalControl::Continue; };
        if (action == Action::Integrity) {
            const auto report = IncrementalRepository::checkIntegrity(request, provider, true);
            if (!BackupRunStore(runPath).finishIntegrityCheck(setId, request.remoteSetRoot, report, &error)) return failure(error);
            return QJsonObject{{"message", "Full integrity status: " + report.value("status").toString()
                + ". This does not save a new state or repair historical snapshots."}};
        }
        QVector<RemoteCopy> copies;
        if (!BackupCatalog::discoverCopies(provider, request.remoteSetRoot + "/snapshots", request.scan.setId, &copies, &error)) return failure(error);
        if (copies.isEmpty()) return failure("No saved incremental snapshot is available for cleanup.");
        request.operationId = request.snapshotId = copies.first().copyId;
        const bool confirmed = action == Action::Confirm;
        const bool dismiss = action == Action::Dismiss;
        if (confirmed) request.cleanupConfirmation = confirmation;
        if (dismiss) request.cleanupDismissal = confirmation;
        QJsonObject proposal;
        QString digest;
        const bool success = IncrementalRepository::cleanup(request, provider, &proposal, &digest, &error);
        if (!confirmed && !dismiss && !proposal.isEmpty() && !digest.isEmpty())
            return QJsonObject{{"set_id", setId}, {"proposal", proposal}, {"confirmation", digest}};
        if (!success) return failure(error.isEmpty() ? "Cleanup requires a fresh exact-proposal preview." : error);
        if (!dismiss && !BackupRunStore(runPath).finishIncrementalCleanup(setId, request.remoteSetRoot, &error))
            return failure("Cleanup completed, but its local attention status could not be updated: " + error);
        return QJsonObject{{"message", dismiss ? "Cleanup proposal dismissed without deleting anything." : confirmed
            ? "Confirmed cleanup completed. Retained and protected references were preserved." : "No cleanup is needed for the current newest-N policy."}};
    }));
}
