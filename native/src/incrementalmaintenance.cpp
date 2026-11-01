#include "incrementalmaintenance.h"
#include "incrementalworker.h"
#include "protonprovider.h"
#include "qprocessrunner.h"
#include "backupcatalog.h"
#include "backuprunstore.h"
#include "incrementalmanifest.h"
#include "incrementalrestore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QLocale>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>
#include <limits>

namespace {
bool savedVersions(BackupProvider &provider, const IncrementalWriteRequest &request, QVector<RemoteCopy> *copies, QString *error)
{
    const auto root = request.remoteSetRoot + "/snapshots";
    QVector<RemoteItem> folders;
    if (!provider.listComplete(root, &folders, error)) return false;
    QSet<QString> seen;
    for (const auto &folder : folders) {
        if (!folder.directory || !IncrementalManifest::validId(folder.name) || folder.path != root + '/' + folder.name
            || seen.contains(folder.name)) {
            *error = "Ambiguous snapshot inventory. No local latest-backup link was changed.";
            return false;
        }
        seen.insert(folder.name);
        const auto presence = provider.lookup(folder.path + "/manifest.json", error);
        if (presence == RemoteLookup::Missing) continue;
        if (presence == RemoteLookup::Unknown) return false;
        RemoteCopy copy;
        if (!IncrementalRestore::describeCopy(provider, folder.path, request.scan.setId, &copy, error)) return false;
        copies->append(copy);
    }
    std::sort(copies->begin(), copies->end(), [](const auto &a, const auto &b) {
        return a.createdAt == b.createdAt ? a.copyId > b.copyId : a.createdAt > b.createdAt;
    });
    error->clear();
    return true;
}
}

IncrementalMaintenance::IncrementalMaintenance(QString path, QObject *parent)
    : QObject(parent), configPath(std::move(path))
{
    connect(&watcher, &QFutureWatcher<QJsonObject>::finished, this, [this] {
        const auto result = watcher.result();
        if (!result.value("history_set").toString().isEmpty()) emit historyChanged(result.value("history_set").toString());
        working = false;
        checkingSet.clear();
        emit busyChanged();
        if (!result.value("error").toString().isEmpty()) {
            pendingSet.clear(); pendingConfirmation.clear();
            emit failed(result.value("error").toString());
        } else if (result.contains("versions")) {
            emit versionsReady(result.value("set_id").toString(), result.value("versions").toArray().toVariantList());
        } else if (result.contains("proposal")) {
            pendingSet = result.value("set_id").toString();
            pendingConfirmation = result.value("confirmation").toString();
            const auto proposal = result.value("proposal").toObject();
            QStringList lines;
            if (result.value("all_backups").toBool())
                lines.append(tr("Warning: this removes all saved backups in this set."));
            const auto snapshots = proposal.value("snapshots").toArray();
            const auto legacy = proposal.value("legacy").toArray();
            lines.append(tr("Backups to delete: %1").arg(snapshots.size() + legacy.size()));
            for (const auto &value : snapshots) {
                const auto snapshot = value.toObject();
                lines.append(tr("• %1 — %2").arg(QDateTime::fromString(snapshot.value("created_at").toString(), Qt::ISODateWithMs)
                    .toLocalTime().toString("dd/MM/yyyy HH:mm:ss"), snapshot.value("snapshot_id").toString()));
            }
            for (const auto &value : legacy) lines.append(tr("• Legacy backup: %1").arg(value.toObject().value("path").toString()));
            if (snapshots.isEmpty() && legacy.isEmpty()) lines.append(tr("No saved backups will be deleted; only unused data."));
            qint64 bytes = 0;
            bool knownSize = legacy.isEmpty();
            const auto objects = proposal.value("objects").toArray();
            for (const auto &value : objects) {
                const auto size = value.toObject().value("size").toInteger(-1);
                if (size < 0 || size > std::numeric_limits<qint64>::max() - bytes) { knownSize = false; break; }
                bytes += size;
            }
            lines.append(QString());
            lines.append(knownSize ? tr("Estimated Proton storage recovered: %1").arg(QLocale().formattedDataSize(bytes))
                : tr("Estimated Proton storage recovered: unavailable"));
            lines.append(tr("Unused stored archives to remove: %1").arg(objects.size()));
            lines.append(tr("This estimate counts stored archives no remaining backup needs, not the total file sizes. Shared data needed by remaining backups stays. Small metadata and Proton quota accounting may differ."));
            emit cleanupReady(pendingSet, lines.join('\n'));
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
void IncrementalMaintenance::listVersions(const QString &setId)
{
    if (!working && pendingSet.isEmpty()) start(setId, Action::Versions);
}
void IncrementalMaintenance::previewDelete(const QString &setId, const QString &snapshotId)
{
    if (!working && pendingSet.isEmpty()) start(setId, Action::Delete, {}, false,
        snapshotId.isEmpty() ? QStringList() : QStringList{snapshotId});
}
void IncrementalMaintenance::previewDeleteVersions(const QString &setId, const QStringList &snapshotIds)
{
    if (working || !pendingSet.isEmpty()) return;
    if (snapshotIds.isEmpty()) { emit failed(tr("Select at least one saved backup to delete.")); return; }
    start(setId, Action::Delete, {}, false, snapshotIds);
}
void IncrementalMaintenance::cancelCleanup()
{
    if (!working && !pendingSet.isEmpty() && !pendingConfirmation.isEmpty())
        start(pendingSet, Action::Dismiss, pendingConfirmation);
}
void IncrementalMaintenance::takeover(const QString &setId, bool disabled) { if (!working) start(setId, Action::Takeover, {}, disabled); }
void IncrementalMaintenance::checkIntegrity(const QString &setId) { if (!working) start(setId, Action::Integrity); }

void IncrementalMaintenance::start(const QString &setId, Action action, const QString &confirmation, bool oldWriterDisabled,
    const QStringList &snapshotIds)
{
    working = true;
    checkingSet = action == Action::Integrity ? setId : QString();
    activeSet = setId;
    switch (action) {
    case Action::Preview: activity = tr("Reviewing old backups and unused data…"); break;
    case Action::Delete: activity = tr("Preparing backup deletion…"); break;
    case Action::Versions: activity = tr("Loading saved backup versions…"); break;
    case Action::Confirm: activity = tr("Deleting confirmed backups and unused data…"); break;
    case Action::Dismiss: activity = tr("Dismissing removal preview…"); break;
    case Action::Takeover: activity = tr("Moving backup responsibility to this computer…"); break;
    case Action::Integrity: activity.clear(); break; // Dedicated integrity feedback.
    }
    stopped->store(false);
    emit busyChanged();
    const auto path = configPath;
    const auto stop = stopped;
    watcher.setFuture(QtConcurrent::run([path, setId, action, oldWriterDisabled, confirmation, snapshotIds, stop] {
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
            if (!BackupConfigStore(path).grantWriteAuthority(setId, set->remoteRoot, &error)) return failure(error);
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
        if (!savedVersions(provider, request, &copies, &error)) return failure(error);
        if (action == Action::Versions) {
            QJsonArray versions;
            for (const auto &copy : copies) {
                versions.append(QJsonObject{{"id", copy.copyId},
                    {"date", copy.createdAt.toLocalTime().toString("dd/MM/yyyy HH:mm:ss")}, {"path", copy.rootPath},
                    {"fileSize", copy.fileSize >= 0 ? QLocale().formattedDataSize(copy.fileSize) : QString()}});
            }
            return QJsonObject{{"set_id", setId}, {"versions", versions}};
        }
        // Confirmation/recovery must also work after the last manifest was removed.
        if (copies.isEmpty() && action == Action::Delete)
            return failure("No saved incremental backup is available.");
        if (!copies.isEmpty()) request.operationId = request.snapshotId = copies.first().copyId;
        if (action == Action::Delete) {
            if (snapshotIds.size() <= 1) request.deleteSnapshotId = snapshotIds.isEmpty() ? copies.first().copyId : snapshotIds.first();
            else request.deleteSnapshotIds = snapshotIds;
        }
        const bool confirmed = action == Action::Confirm;
        const bool dismiss = action == Action::Dismiss;
        if (confirmed) request.cleanupConfirmation = confirmation;
        if (dismiss) request.cleanupDismissal = confirmation;
        QJsonObject proposal;
        QString digest;
        const bool success = IncrementalRepository::cleanup(request, provider, &proposal, &digest, &error);
        const bool historyMayHaveChanged = confirmed || (success && !dismiss && !proposal.isEmpty());
        if (historyMayHaveChanged) {
            QVector<RemoteCopy> remaining;
            QString refreshError;
            if (!savedVersions(provider, request, &remaining, &refreshError)
                || !BackupRunStore(runPath).reconcileSnapshotHistory(setId, request.remoteSetRoot,
                    remaining.isEmpty() ? QString() : remaining.first().rootPath,
                    remaining.isEmpty() ? QDateTime() : remaining.first().createdAt, &refreshError))
                return QJsonObject{{"error", refreshError}, {"history_set", setId}};
        }
        if (!success && !confirmed && !dismiss && !proposal.isEmpty() && !digest.isEmpty()
            && error == "Review the exact cleanup proposal and confirm its SHA-256 before deletion.") {
            if (action == Action::Delete) {
                auto selected = proposal.contains("selected_snapshot") ? QStringList{proposal.value("selected_snapshot").toString()}
                    : proposal.value("selected_snapshots").toVariant().toStringList();
                auto requested = request.deleteSnapshotIds;
                if (!request.deleteSnapshotId.isEmpty()) requested.append(request.deleteSnapshotId);
                selected.sort(); requested.sort();
                if (selected != requested)
                    return failure("Another cleanup proposal is pending. Open Manage saved backups and Review suggested cleanup first, then retry this deletion.");
            }
            return QJsonObject{{"set_id", setId}, {"proposal", proposal}, {"confirmation", digest},
                {"all_backups", !copies.isEmpty() && copies.size() == proposal.value("snapshots").toArray().size()}};
        }
        if (!success) return QJsonObject{{"error", error.isEmpty() ? "Cleanup requires a fresh exact-proposal preview." : error},
            {"history_set", historyMayHaveChanged ? setId : QString()}};
        if (!dismiss && !BackupRunStore(runPath).finishIncrementalCleanup(setId, request.remoteSetRoot, &error))
            return failure("Cleanup completed, but its local attention status could not be updated: " + error);
        return QJsonObject{{"history_set", historyMayHaveChanged ? setId : QString()}, {"message", dismiss ? "Cleanup proposal dismissed without deleting anything." : historyMayHaveChanged
            ? "Confirmed cleanup completed. Retained and protected references were preserved." : "No cleanup is needed for the current newest-N policy."}};
    }));
}
